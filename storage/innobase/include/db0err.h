/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/db0err.h
Global error codes for the database

Created 5/24/1996 Heikki Tuuri
*******************************************************/

#ifndef db0err_h
#define db0err_h

/* Do not include univ.i because univ.i includes this. */

enum dberr_t {
	DB_SUCCESS,

	DB_SUCCESS_LOCKED_REC= 9,		/*!< like DB_SUCCESS, but a new
					explicit record lock was created */

	/* The following are error codes */
	DB_RECORD_CHANGED,
	DB_ERROR,
	DB_INTERRUPTED,
	DB_OUT_OF_MEMORY,
	DB_OUT_OF_FILE_SPACE,
	DB_LOCK_WAIT,
	DB_DEADLOCK,
	DB_DUPLICATE_KEY,
	DB_MISSING_HISTORY,		/*!< required history data has been
					deleted due to lack of space in
					rollback segment */
#ifdef WITH_WSREP
	DB_ROLLBACK,
#endif
	DB_TABLE_NOT_FOUND= 31,
	DB_TOO_BIG_RECORD,		/*!< a record in an index would not fit
					on a compressed page, or it would
					become bigger than 1/2 free space in
					an uncompressed page frame */
	DB_LOCK_WAIT_TIMEOUT,		/*!< lock wait lasted too long */
	DB_NO_REFERENCED_ROW,		/*!< referenced key value not found
					for a foreign key in an insert or
					update of a row */
	DB_ROW_IS_REFERENCED,		/*!< cannot delete or update a row
					because it contains a key value
					which is referenced */
	DB_CANNOT_ADD_CONSTRAINT,	/*!< adding a foreign key constraint
					to a table failed */
	DB_CORRUPTION,			/*!< data structure corruption
					noticed */
	DB_CANNOT_DROP_CONSTRAINT,	/*!< dropping a foreign key constraint
					from a table failed */
	DB_TABLESPACE_EXISTS,		/*!< we cannot create a new single-table
					tablespace because a file of the same
					name already exists */
	DB_TABLESPACE_DELETED,		/*!< tablespace was deleted or is
					being dropped right now */
	DB_TABLESPACE_NOT_FOUND,	/*<! Attempt to delete a tablespace
					instance that was not found in the
					tablespace hash table */
	DB_LOCK_TABLE_FULL,		/*!< lock structs have exhausted the
					buffer pool (for big transactions,
					InnoDB stores the lock structs in the
					buffer pool) */
	DB_FOREIGN_DUPLICATE_KEY,	/*!< foreign key constraints
					activated by the operation would
					lead to a duplicate key in some
					table */
	DB_TOO_MANY_CONCURRENT_TRXS,	/*!< when InnoDB runs out of the
					preconfigured undo slots, this can
					only happen when there are too many
					concurrent transactions */
	DB_UNSUPPORTED,			/*!< when InnoDB sees any artefact or
					a feature that it can't recognize or
					work with e.g., FT indexes created by
					a later version of the engine. */

	DB_INVALID_NULL,		/*!< a NOT NULL column was found to
					be NULL during table rebuild */

	DB_STATS_DO_NOT_EXIST,		/*!< an operation that requires the
					persistent storage, used for recording
					table and index statistics, was
					requested but this storage does not
					exist itself or the stats for a given
					table do not exist */
	DB_FOREIGN_EXCEED_MAX_CASCADE,	/*!< Foreign key constraint related
					cascading delete/update exceeds
					maximum allowed depth */
	DB_CHILD_NO_INDEX,		/*!< the child (foreign) table does
					not have an index that contains the
					foreign keys as its prefix columns */
	DB_PARENT_NO_INDEX,		/*!< the parent table does not
					have an index that contains the
					foreign keys as its prefix columns */
	DB_TOO_BIG_INDEX_COL,		/*!< index column size exceeds
					maximum limit */
	DB_INDEX_CORRUPT,		/*!< we have corrupted index */
	DB_UNDO_RECORD_TOO_BIG,		/*!< the undo log record is too big */
	DB_READ_ONLY,			/*!< Update operation attempted in
					a read-only transaction */
	DB_FTS_INVALID_DOCID,		/* FTS Doc ID cannot be zero */
	DB_ONLINE_LOG_TOO_BIG,		/*!< Modification log grew too big
					during online index creation */

	DB_IDENTIFIER_TOO_LONG,		/*!< Identifier name too long */
	DB_FTS_EXCEED_RESULT_CACHE_LIMIT,	/*!< FTS query memory
					exceeds result cache limit */
	DB_TEMP_FILE_WRITE_FAIL,	/*!< Temp file write failure */
	DB_CANT_CREATE_GEOMETRY_OBJECT,	/*!< Cannot create specified Geometry
					data object */
	DB_CANNOT_OPEN_FILE,		/*!< Cannot open a file */
	DB_FTS_TOO_MANY_WORDS_IN_PHRASE,
					/*< Too many words in a phrase */

	DB_DECRYPTION_FAILED,		/* Tablespace encrypted and
					decrypt operation failed because
					of missing key management plugin,
					or missing or incorrect key or
					incorrect AES method or algorithm. */

	DB_IO_ERROR = 100,		/*!< Generic IO error */

	DB_IO_PARTIAL_FAILED,		/*!< Partial IO request failed */

	DB_TABLE_CORRUPT,		/*!< Table/clustered index is
					corrupted */

	DB_COMPUTE_VALUE_FAILED,	/*!< Compute generated value failed */

	DB_NO_FK_ON_S_BASE_COL,		/*!< Cannot add foreign constrain
					placed on the base column of
					stored column */

	DB_IO_NO_PUNCH_HOLE,		/*!< Punch hole not supported by
					file system. */

	DB_PAGE_CORRUPTED,		/* Page read from tablespace is
					corrupted. */
	/* The following are partial failure codes */
	DB_FAIL = 1000,
	DB_OVERFLOW,
	DB_UNDERFLOW,
	DB_STRONG_FAIL,
	DB_ZIP_OVERFLOW,
	DB_RECORD_NOT_FOUND = 1500,
	DB_END_OF_INDEX,
	DB_NOT_FOUND,			/*!< Generic error code for "Not found"
					type of errors */
};

#endif
