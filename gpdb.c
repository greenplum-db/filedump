/*-------------------------------------------------------------------------
 *
 * gpdb.c
 *	  Dump append only table
 *
 *
 * Copyright (c) 2021 VMware, Inc. or its affiliates
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 *
 *-------------------------------------------------------------------------
 */
#include "pg_filedump.h"

#include "gpdb.h"

#include "cdb/cdbappendonlystorageformat.h"
#include "cdb/cdbappendonlystorageformat_impl.h"
#include "utils/datumstreamblock.h"

#if GP_VERSION_NUM >= 60000
#include "access/gin_private.h"
#endif /* GP_VERSION_NUM */

#ifdef ENABLE_ZLIB
#include <zlib.h>
#endif /* ENABLE_ZLIB */

#ifdef ENABLE_ZSTD
#include <zstd.h>
#include <zstd_errors.h>
#endif /* ENABLE_ZSTD */

#ifdef ENABLE_QUICKLZ
#if GP_VERSION_NUM >= 60000
#include "quicklz.h"
#else /* GP_VERSION_NUM */
#include "quicklz1.h"
#include "quicklz3.h"
#endif /* GP_VERSION_NUM */
#endif /* ENABLE_QUICKLZ */

/*
 * From the QuickLZ manual:
 *
 *   "The destination buffer must be at least size + 400 bytes large because
 *   incompressible data may increase in size."
 *
 */
#define EXTRA_BUFFER_SPACE_REQUIRED_FOR_QUICKLZ 400

/* Variables for AppendOnly Column Oriented table Dump */
/* FIXME: GPDB: could we always use the max supported version? */
#define AO_CO_HEADER_VERSION (MaxAORelationVersion - 1)

#if 0
/* FIXME: GPDB: used by disabled options */
static TupleDesc	gpdbTupdesc = NULL;

static char		   *data_dir = NULL;/* data directory */
static char		   *commit_log_dir = NULL;	/* clog */
static Oid			relid = InvalidOid;
#endif

/* GPDB options */
unsigned int gpdbOptions = 0;


static void
Print_AppendOnlyStorageReadCurrent(AppendOnlyStorageReadCurrent *blkHdr)
{
	printf("\n executorBlockKind: %d rowCount: %d\n headerLen: %d uncompressedLen: %d",
		   blkHdr->executorBlockKind, blkHdr->rowCount,
		   blkHdr->actualHeaderLen, blkHdr->uncompressedLen);

	if (blkHdr->isCompressed)
		printf(" compressedLen: %d", blkHdr->compressedLen);
	else
		printf(" (Block Not Compressed)");

	printf("\n firstRowNumber: ");
	if (blkHdr->hasFirstRowNum)
		printf(INT64_FORMAT, blkHdr->firstRowNum);
	else
		printf("NONE");
}

static bool
Dump_AppendOnlyStorage_ContentHeaderInfo(uint8 *headerPtr,
										 AppendOnlyStorageReadCurrent *blkHdr)
{
	AOHeaderCheckError checkError;
	pg_crc32	storedChecksum = 0;
	pg_crc32	computedChecksum = 0;
	bool		hasChecksum = gpdbOptions & GPDB_HAS_CHECKSUM;
	bool		verifyChecksum = blockOptions & BLOCK_CHECKSUMS;
	bool		success = true;
	int			version = AO_CO_HEADER_VERSION;

	/* refer to AppendOnlyStorageRead_ReadNextBlock() */
	checkError = AppendOnlyStorageFormat_GetHeaderInfo(headerPtr,
													   hasChecksum,
													   &blkHdr->headerKind,
													   &blkHdr->actualHeaderLen);
	/* TODO: check checkError */

	printf("\n<AO Header> -----");
	printf("\n HeaderKind: %d ", blkHdr->headerKind);

	if (hasChecksum && verifyChecksum)
	{
		if (!AppendOnlyStorageFormat_VerifyHeaderChecksum(headerPtr,
														  &storedChecksum,
														  &computedChecksum))
		{
			printf("\n Header checksum's don't match (StoredChecksum 0x%08x, ComputedChecksum 0x%08x)\n",
				   storedChecksum, computedChecksum);
			return false;
		}
		printf("\n Header Checksum Valid (checksum 0x%08x)\n", storedChecksum);
	}

	switch (blkHdr->headerKind)
	{
		case AoHeaderKind_None:
			printf("(AoHeaderKind_None)");
			success = false;
			break;

		case AoHeaderKind_SmallContent:
			printf("(AoHeaderKind_SmallContent)");
			checkError = AppendOnlyStorageFormat_GetSmallContentHeaderInfo
				(headerPtr,
				 blkHdr->actualHeaderLen,
				 hasChecksum,
				 MAX_APPENDONLY_BLOCK_SIZE,
				 &blkHdr->overallBlockLen,
				 &blkHdr->contentOffset,
				 &blkHdr->uncompressedLen,
				 &blkHdr->executorBlockKind,
				 &blkHdr->hasFirstRowNum,
				 version,
				 &blkHdr->firstRowNum,
				 &blkHdr->rowCount,
				 &blkHdr->isCompressed,
				 &blkHdr->compressedLen);
			break;

		case AoHeaderKind_LargeContent:
			printf("AoHeaderKind_LargeContent");
			checkError = AppendOnlyStorageFormat_GetLargeContentHeaderInfo
				(headerPtr,
				 blkHdr->actualHeaderLen,
				 hasChecksum,
				 &blkHdr->uncompressedLen,
				 &blkHdr->executorBlockKind,
				 &blkHdr->hasFirstRowNum,
				 &blkHdr->firstRowNum,
				 &blkHdr->rowCount);

			blkHdr->overallBlockLen = blkHdr->actualHeaderLen;
			blkHdr->contentOffset = blkHdr->actualHeaderLen;
			blkHdr->isLarge = true;
			break;

		case AoHeaderKind_NonBulkDenseContent:
			printf("AoHeaderKind_NonBulkDenseContent");
			checkError = AppendOnlyStorageFormat_GetNonBulkDenseContentHeaderInfo
				(headerPtr,
				 blkHdr->actualHeaderLen,
				 hasChecksum,
				 MAX_APPENDONLY_BLOCK_SIZE,
				 &blkHdr->overallBlockLen,
				 &blkHdr->contentOffset,
				 &blkHdr->uncompressedLen,
				 &blkHdr->executorBlockKind,
				 &blkHdr->hasFirstRowNum,
				 version,
				 &blkHdr->firstRowNum,
				 &blkHdr->rowCount);
			break;

		case AoHeaderKind_BulkDenseContent:
			printf("AoHeaderKind_BulkDenseContent");
			checkError = AppendOnlyStorageFormat_GetBulkDenseContentHeaderInfo
				(headerPtr,
				 blkHdr->actualHeaderLen,
				 hasChecksum,
				 MAX_APPENDONLY_BLOCK_SIZE,
				 &blkHdr->overallBlockLen,
				 &blkHdr->contentOffset,
				 &blkHdr->uncompressedLen,
				 &blkHdr->executorBlockKind,
				 &blkHdr->hasFirstRowNum,
				 version,
				 &blkHdr->firstRowNum,
				 &blkHdr->rowCount,
				 &blkHdr->isCompressed,
				 &blkHdr->compressedLen);
			break;

		default:
			printf("Unexpected Append-Only header kind %d", blkHdr->headerKind);
			success = false;
			break;
	}

	if (checkError != AOHeaderCheckOk)
	{
		printf("\nError append-only storage header of type %d. Header check error\n",
			   blkHdr->headerKind);
		success = false;
	}
	else
		Print_AppendOnlyStorageReadCurrent(blkHdr);

	return success;
}

static bool
Dump_AppendOnlyStorage_ContentInfo(uint8 *headerPtr,
								   AppendOnlyStorageReadCurrent *blkHdr)
{
	DatumStreamBlock_Orig *contentPtr;
	Bytef	   *uncompressedDatum = NULL;
	bool		success = true;

	printf("\n\t<AO Content Info> -----");

	if (blkHdr->isCompressed)
	{
		int			compressType = GPDB_GET_COMPRESS_TYPE(gpdbOptions);
		int			compressLevel = GPDB_GET_COMPRESS_LEVEL(gpdbOptions);

		uncompressedDatum = palloc(blkHdr->uncompressedLen +
								   EXTRA_BUFFER_SPACE_REQUIRED_FOR_QUICKLZ);
		printf("\n\t Decompressing using \"%s\" with compressionLevel: %d",
			   compressType == GPDB_COMPRESS_TYPE_ZLIB ? "ZLIB" :
			   compressType == GPDB_COMPRESS_TYPE_QUICKLZ ? "QUICKLZ" :
			   compressType == GPDB_COMPRESS_TYPE_ZSTD ? "ZSTD" :
			   "UNKNOWN",
			   compressLevel);

#ifdef ENABLE_QUICKLZ
		if (compressType == GPDB_COMPRESS_TYPE_QUICKLZ)
		{
			uLongf		destLen = 0;

#ifdef ENABLE_QUICKLZ_LEGACY
			if (compressLevel == 1)
			{
				qlz1_state_decompress state;

				memset(&state, 0, sizeof(qlz1_state_decompress));
				destLen = qlz1_decompress((char *) headerPtr,
										  uncompressedDatum, &state);
			}

			if (compressLevel == 3)
			{
				qlz3_state_decompress state;

				memset(&state, 0, sizeof(qlz3_state_decompress));
				destLen = qlz3_decompress((char *) headerPtr,
										  uncompressedDatum, &state);
			}
#else /* ENABLE_QUICKLZ_LEGACY */
			{
				qlz_state_decompress state;

				memset(&state, 0, sizeof(state));
				destLen = qlz_decompress((char *) headerPtr,
										 uncompressedDatum,
										 &state);
			}
#endif /* ENABLE_QUICKLZ_LEGACY */

			if (destLen != blkHdr->uncompressedLen)
			{
				printf("\n\t Decompression falied for QuickLZ as expected uncompressedLen: %d and actual uncompressedLen: %lu",
					   blkHdr->uncompressedLen, destLen);
				pfree(uncompressedDatum);
				return false;
			}
		}
#endif /* ENABLE_QUICKLZ */

#ifdef ENABLE_ZLIB
		if (compressType == GPDB_COMPRESS_TYPE_ZLIB)
		{
			uLongf		destLen = blkHdr->uncompressedLen;
			int			status;

			status = uncompress(uncompressedDatum, &destLen,
								(Bytef *) headerPtr, blkHdr->compressedLen);

			if (status != Z_OK)
			{
				printf("\nERROR uncompression failed: %s(%d)  %lu",
					   zError(status), status, destLen);
				pfree(uncompressedDatum);
				return false;
			}
		}
#endif /* ENABLE_ZLIB */

#ifdef ENABLE_ZSTD
		if (compressType == GPDB_COMPRESS_TYPE_ZSTD)
		{
			uLongf		destLen = blkHdr->uncompressedLen;

			destLen = ZSTD_decompress(uncompressedDatum, destLen,
									  headerPtr, blkHdr->compressedLen);

			if (ZSTD_isError(destLen))
			{
				printf("\nERROR uncompression failed: %s(%d)  %lu",
					   ZSTD_getErrorName(destLen),
					   ZSTD_getErrorCode(destLen),
					   destLen);
				pfree(uncompressedDatum);
				return false;
			}
		}
#endif /* ENABLE_ZSTD */

		headerPtr = uncompressedDatum;
	}

	contentPtr = (DatumStreamBlock_Orig *) headerPtr;

	switch (contentPtr->version)
	{
		case DatumStreamVersion_Original:
			printf("\n\t DatumStreamVersion_Original  flags: %d",
				   contentPtr->flags);
			if (contentPtr->flags & DSB_HAS_NULLBITMAP)
				printf(" (HAS_NULLBITMAP)");
			printf("\n\t ndatum: %d nullSize: %d Size: %d",
				   contentPtr->ndatum, contentPtr->nullsz, contentPtr->sz);
			break;

		case DatumStreamVersion_Dense:
		case DatumStreamVersion_Dense_Enhanced:
			{
				uint8	   *p = headerPtr;
				DatumStreamBlock_Dense *densePtr = (DatumStreamBlock_Dense *) p;

				p += sizeof(DatumStreamBlock_Dense);
				printf("\n\t DatumStreamVersion_Dense  flags: %d",
					   densePtr->orig_4_bytes.flags);
				/* Print meaning of flags before printing the header fields. */
				if (densePtr->orig_4_bytes.flags & DSB_HAS_NULLBITMAP)
					printf(" (HAS_NULLBITMAP)");
				if (densePtr->orig_4_bytes.flags & DSB_HAS_RLE_COMPRESSION)
					printf(" (HAS_RLE_COMPRESSION)");
				if (densePtr->orig_4_bytes.flags & DSB_HAS_DELTA_COMPRESSION)
					printf(" (HAS_DELTA_COMPRESSION)");
				printf("\n\t logical_row_count: %d physical_datum_count: %d"
					   "\n\t physical_data_size: %d",
					   densePtr->logical_row_count,
					   densePtr->physical_datum_count,
					   densePtr->physical_data_size );
				/* Print RLE and/or Delta Extension Headers if present. */
				if (densePtr->orig_4_bytes.flags & DSB_HAS_RLE_COMPRESSION)
				{
					DatumStreamBlock_Rle_Extension *rlePtr =
						(DatumStreamBlock_Rle_Extension *) p;

					p += sizeof(DatumStreamBlock_Rle_Extension);
					printf("\n\t\t<RLE Extension> -----");
					printf("\n\t\t norepeats_null_bitmap_count: %d compress_bitmap_count: %d"
						   "\n\t\t repeatcounts_count: %d repeatcounts_size: %d",
						   rlePtr->norepeats_null_bitmap_count,
						   rlePtr->compress_bitmap_count,
						   rlePtr->repeatcounts_count,
						   rlePtr->repeatcounts_size);
				}
				if (densePtr->orig_4_bytes.flags & DSB_HAS_DELTA_COMPRESSION)
				{
					DatumStreamBlock_Delta_Extension *deltaPtr =
						(DatumStreamBlock_Delta_Extension *) p;

					printf("\n\t\t<DELTA Extension> ----");
					printf("\n\t\t delta_bitmap_count: %d deltas_count: %d deltas_size: %d",
						   deltaPtr->delta_bitmap_count,
						   deltaPtr->deltas_count,
						   deltaPtr->deltas_size);
				}
				break;
			}

		default:
			printf("\n Unexpected Append-Only Content DatumStream Version %d",
				   contentPtr->version);
			success = false;;
	}

	if (uncompressedDatum != NULL)
		pfree(uncompressedDatum);

	return success;
}

#if 0
/* FIXME: GPDB: used by -p */
/*
 * Given a path to the file we're interested in, find the clog and general
 * data directory.
 */
void
file_set_dirs(char *file)
{
	int             len = strlen(file);
	int             pos = len;
	int             seps_seen = 0;	/* how man seperators have we seen? */

	if (file[0] != '/')
	{
		fprintf(stderr, "Filename %s should be absolute pathname\n", file);
		exit(1);
	}

	for (pos = len - 1; pos >= 0; pos--)
	{
		/* Fix for WIN32 */
		if (file[pos] == '/')
		{
			seps_seen++;
			if (seps_seen == 1)
			{
				/* must be data directory */

				/* could this be a global directory? */
#define GSTR "global/"
#define GSTRLEN strlen(GSTR)
				if (pos + 1 >= GSTRLEN)
				{
					if (strncmp(&(file[pos + 1 - GSTRLEN]), GSTR, GSTRLEN) == 0)
					{
						/*
						 * It is the global directory, so backup and make the
						 * data directory template0.
						 */
						data_dir = malloc((pos + 1 - GSTRLEN + 6 + 1) *
										  sizeof(char));
						strncpy(data_dir, file, pos + 1 - GSTRLEN);
						strcat(data_dir, "base/1");
					}
					else
					{
						data_dir = malloc((pos + 1) * sizeof(char));
						strncpy(data_dir, file, pos);
					}
				}
				else
				{
					data_dir = malloc((pos + 1) * sizeof(char));
					strncpy(data_dir, file, pos);
				}
				relid = atol(&file[pos + 1]);
			}
			else if (seps_seen == 3)
			{
				/* must be base data directory */
				commit_log_dir = malloc((pos + strlen("/pg_clog") + 1) * sizeof(char));
				strncpy(commit_log_dir, file, pos);
				strcat(commit_log_dir, "/pg_clog");
				DIR *dir = opendir(commit_log_dir);
				if (dir == NULL)
				{
					fprintf(stderr, "Utility should be run from the base data directory\n");
					exit(1);
				}
                closedir(dir);
				break;
			}
		}
	}

	if (data_dir == NULL || commit_log_dir == NULL)
	{
		fprintf(stderr, "could not extract data directories from %s\n", file);
		exit(1);
	}
}

/*
 * Build a tuple descriptor for the relation. This involves scanning
 * and building a valid Form_pg_attribute.
 */
void
build_tupdesc(void)
{
	Form_pg_attribute *attrs;	/* XXX: make this dynamic */
	int             segno = 0;
	AttrNumber      maxattno = 0;

	attrs = malloc(sizeof(Form_pg_attribute) * 100);
	/* loop over each segno */
	do
	{
		char            segfile[MAXPGPATH];
		FILE           *seg;
		char            ext[10];

		if (segno > 0)
			sprintf(ext, ".%i", segno);
		else
			ext[0] = '\0';


		snprintf(segfile, sizeof(segfile), "%s/%u%s",
			 data_dir, AttributeRelationId, ext);
		seg = fopen(segfile, "r");

		if (!seg)
			break;

		while (!feof(seg))
		{
			char            buf[BLCKSZ];
			size_t          bytes;
			Page            pg;
			int             lines;
			OffsetNumber    lineoff;
			ItemId          lpp;

			if ((bytes = fread(buf, 1, BLCKSZ, seg)) != BLCKSZ)
			{
				if (bytes == 0)
					break;

				/* XXX: error */
			}
			pg = (Page) buf;
			lines = PageGetMaxOffsetNumber(pg);

			for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(pg, lineoff);
			     lineoff <= lines;
			     lineoff++, lpp++)
			{
				if (ItemIdIsUsed(lpp))
				{
					HeapTupleHeader theader =
					(HeapTupleHeader) PageGetItem((Page) pg, lpp);
					HeapTuple       tuple = malloc(sizeof(HeapTupleData));
					Form_pg_attribute att;

					tuple->t_data = theader;
					att = (Form_pg_attribute) GETSTRUCT(tuple);
					if (att->attrelid == relid && att->attnum > 0)
					{
						Form_pg_attribute attcopy = malloc(ItemIdGetLength(lpp) - theader->t_hoff);
						memcpy(attcopy, att, ItemIdGetLength(lpp) - theader->t_hoff);
						attrs[att->attnum - 1] = attcopy;
						if (att->attnum > maxattno)
							maxattno = att->attnum;
					}
					else
					{
						free(tuple);
					}
				}
			}
		}
	} while (segno++ < 65536);

	gpdbTupdesc = malloc(sizeof(TupleDesc));
	gpdbTupdesc->natts = maxattno;
	gpdbTupdesc->attrs = attrs;
}


/* fix hint bits for checksumming */
void
fixupItemBlock(Page page, unsigned int blockSize, unsigned int bytesToFormat)
{
	char	   *buffer = (char *) page;
	int			maxOffset = PageGetMaxOffsetNumber(page);

	/*
	 * If it's a btree meta page, the meta block is where items would
	 * normally be; don't print garbage.
	 */
	if (IsBtreeMetaPage(page))
		return;

//	printf("<Data> ------\n");

	/* Loop through the items on the block.  Check if the block is */
	/* empty and has a sensible item array listed before running  */
	/* through each item   */
	if (maxOffset == 0)
		printf(" Empty block - no items listed\n\n");
	else if ((maxOffset < 0) || (maxOffset > blockSize))
		printf(" Error: Item index corrupt on block. Offset: <%d>.\n\n",
		       maxOffset);
	else
	{
		for (unsigned int x = 1; x < (maxOffset + 1); x++)
		{
			ItemId		itemId = PageGetItemId(page, x);
			unsigned int itemSize = (unsigned int) ItemIdGetLength(itemId);
			unsigned int itemOffset = (unsigned int) ItemIdGetOffset(itemId);

//			printf(" Item %3u -- Length: %4u  Offset: %4u (0x%04x)"
//			"  Flags: %s\n", x, itemSize, itemOffset, itemOffset,
//			       textFlags);

			/*
			 * Make sure the item can physically fit on this
			 * block before
			 */

			/* formatting */
			if ((itemOffset + itemSize > blockSize) ||
			    (itemOffset + itemSize > bytesToFormat))
				printf("  Error: Item contents extend beyond block.\n"
				       "         BlockSize<%d> Bytes Read<%d> Item Start<%d>.\n",
				       blockSize, bytesToFormat, itemOffset + itemSize);
			else
			{
				unsigned int	numBytes	= itemSize;
				unsigned int	startIndex	= itemOffset;
				int             alignedSize = MAXALIGN(sizeof(HeapTupleHeaderData));

				if (numBytes < alignedSize)
				{
					if (numBytes)
						printf("  Error: This item does not look like a heap item.\n");
				}
				else
				{
					HeapTupleHeader htup =
							(HeapTupleHeader) (&buffer[startIndex]);

					htup->t_infomask = 0;
				}
			}
		}
	}
}
#endif

int
DumpAppendOnlyFileContents(FILE *fp, unsigned int blockSize)
{
	int			orientation = GPDB_GET_ORIENTATION(gpdbOptions);
	char	   *buffer = malloc(blockSize);
	BlockNumber	currentBlock = 0;
	int			bytesRead = 0;
	bool		success = true;
	int64		blocksRemaining = 0;
	bool		largeContentFragments = false;
	unsigned long overallBlockLenDisplay = 0;
	unsigned long long totaloverallBlockPosition = 0;
	bool		hasChecksum = gpdbOptions & GPDB_HAS_CHECKSUM;
	bool		verifyChecksum = blockOptions & BLOCK_CHECKSUMS;
	pg_crc32	storedChecksum = 0;
	pg_crc32	computedChecksum = 0;
#ifdef ENABLE_QUICKLZ_LEGACY
	int			compressType = GPDB_GET_COMPRESS_TYPE(gpdbOptions);
	int			compressLevel = GPDB_GET_COMPRESS_LEVEL(gpdbOptions);

	if (compressType == GPDB_COMPRESS_TYPE_QUICKLZ)
	{
		if (compressLevel != 1 && compressLevel != 3)
		{
			printf("\n compressLevel: %d\n", compressLevel);
			printf("\nQuickLZ compression supports only compression levels 1 or 3\n");
			return 1;
		}
	}
#endif /* ENABLE_QUICKLZ_LEGACY */

	while (success)
	{
		AppendOnlyStorageReadCurrent blkHdr;
		int			seekOffset;

		bytesRead = fread(buffer, 1, blockSize, fp);
		printf("\n Initial Bytes Read = %d\n", bytesRead);
		if (bytesRead == 0)
			break;

		if (!largeContentFragments)
		{
			printf("\n***** Block %4u Start ***( Position: %4llu) *************************",
				   currentBlock, totaloverallBlockPosition);
			overallBlockLenDisplay = 0;
		}

		memset(&blkHdr, 0, sizeof(blkHdr));

		success = Dump_AppendOnlyStorage_ContentHeaderInfo((uint8 *) buffer,
														   &blkHdr);
		if (!success)
		{
			printf("\nError dumping append-only Block Header info.\n");
			break;
		}

		/* refer to AppendOnlyStorageRead_Content() */
		if (!blkHdr.isLarge)
		{
			if (!largeContentFragments)
			{
				/*
				 * If the Datum is not compressed, then actual blockSize
				 * doesn't matter as tool is dumping just the Headers and not
				 * Data. The tool by default is reading 32K and hence headers
				 * fit into this range despite blockSize.
				 *
				 * Only if we have compressed Datum and compressed size if
				 * greater than 32K, we need to grow the buffer and read it at
				 * full to uncompress successfully.
				 */
				if (blkHdr.isCompressed)
				{
					if (blockSize < blkHdr.overallBlockLen)
					{
						blockSize = blkHdr.overallBlockLen;
						free(buffer);
						buffer = (char *) malloc(blockSize);

						/* Move back the filepointer to start of this block */
						if (fseek(fp, -bytesRead, SEEK_CUR) != 0)
						{
							printf("\nError: Seek error encountered.\n" );
							return 1;
						}
						/* Read the block again now in full */
						bytesRead = fread(buffer, 1, blockSize, fp);
						// printf("\n Bytes Read = %d\n", bytesRead);
						if (bytesRead == 0)
						{
							printf("\nFailed to read the Block from file\n");
							return 1;
						}
					}
				}

				if (hasChecksum && verifyChecksum)
				{
					if (!AppendOnlyStorageFormat_VerifyBlockChecksum((uint8 *) buffer,
																	 blkHdr.overallBlockLen,
																	 &storedChecksum,
																	 &computedChecksum))
					{
						printf("\nContent checksum doesn't match storedChecksum: 0x%08x, computedChecksum: 0x%08x\n",
							   storedChecksum, computedChecksum);
						success = false;
					}
					else
						printf("\nContent checksum valid (checksum: 0x%08x)\n",
							   storedChecksum);
				}

				if (success && orientation == GPDB_ORIENTATION_COLUMN)
				{
					if (!Dump_AppendOnlyStorage_ContentInfo((uint8 *) buffer + blkHdr.contentOffset,
															&blkHdr))
					   printf("\nError dumping append-only Block Content info, still moving ahead to dump rest\n");
				}
				success = true;
			}
			else
			{
				Assert(blocksRemaining >= blkHdr.uncompressedLen);
				blocksRemaining = blocksRemaining - blkHdr.uncompressedLen;
				if (blocksRemaining == 0)
					largeContentFragments = false;
			}
		}
		else
		{
			/* Recurse till end of Large content */
			blocksRemaining = blkHdr.uncompressedLen;
			largeContentFragments = true;
		}

		overallBlockLenDisplay = overallBlockLenDisplay + blkHdr.overallBlockLen;

		if (!largeContentFragments)
		{
			printf("\n***** Block %4u End (Total Block length: %4lu) ******************",
				   currentBlock, overallBlockLenDisplay);
			currentBlock++;
			totaloverallBlockPosition = totaloverallBlockPosition + overallBlockLenDisplay;
		}

		/*
		 * We have already seeked fp by blockSize.
		 *
		 * So, now just need to move it by the difference between current
		 * totalAoBlockSize and blockSize
		 */
		if (bytesRead > blkHdr.overallBlockLen)
		{
			/* Seek backward */
			seekOffset = bytesRead - blkHdr.overallBlockLen;
			if (fseek(fp, -seekOffset, SEEK_CUR) != 0)
			{
				printf("Error: Seek error encountered when moving backward...\n");
				success = false;
			}
		}
		else
		{
			/* Seek forward */
			seekOffset = blkHdr.overallBlockLen - bytesRead;
			if (fseek(fp, seekOffset, SEEK_CUR) != 0)
			{
				printf("Error: Seek error encountered when moving forward...\n");
				success = false;
			}
		}
	}

	if (success)
		printf("\n Done DumpAppendOnlyFileContents.....\n");
	else
		printf("\n FAILED to DumpAppendOnlyFileContents.....\n");

	return success ? 0 : 1;
}

#if 0
void
DeparseMiniPage(struct Minipage *minipage, uint32 numEntries, char *deparsed)
{
	int start_no, end_no;

	if (minipage == NULL)
		return;

	strcat(deparsed, "    Entries per page:");
	strcat(deparsed, "\n\tFirstRowNum  FileOffSet  RowCount");
	strcat(deparsed, "\n\t--------------------------------\n");

	start_no = 0;
	Assert(numEntries > 0);
	end_no = numEntries - 1;
	MinipageEntry *entry;
	char           *tmp = malloc(64);
	Assert(tmp != NULL);
	while (start_no <= end_no)
	{
		entry = &(minipage->entry[start_no]);

		Assert(entry->firstRowNum > 0);
		Assert(entry->rowCount > 0);

		Assert(entry != NULL);
		pg_ltoa(DatumGetInt64(entry->firstRowNum), tmp);
		strcat(deparsed, "\t");
		strcat(deparsed, tmp);
		strcat(deparsed, "\t");
		pg_ltoa(DatumGetInt64(entry->fileOffset), tmp);
		strcat(deparsed, tmp);
		strcat(deparsed, "\t");
		pg_ltoa(DatumGetInt64(entry->rowCount), tmp);
		strcat(deparsed, tmp);
		strcat(deparsed, "\n");

		start_no = start_no + 1;

	}
	if (tmp)
		free(tmp);
}
#endif
