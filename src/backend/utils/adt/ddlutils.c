/*-------------------------------------------------------------------------
 *
 * ddlutils.c
 *	  Functions to reconstruct DDL statements from catalog data.
 *
 * Unlike ruleutils.c (which deparses expressions and query trees),
 * these functions generate DDL by reading catalog attributes directly.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddlutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdarg.h>

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* Pretty flags (subset needed for DDL formatting) */
#define PRETTYFLAG_INDENT		0x0002

/* DDL Options flags */
#define PG_DDL_PRETTY_INDENT	0x00000001
#define PG_DDL_WITH_DEFAULTS	0x00000002
#define PG_DDL_NO_OWNER			0x00000004
#define PG_DDL_NO_TABLESPACE	0x00000008

/*
 * Structure to define DDL options for parse_ddl_options().
 * This allows easy addition of new options in the future.
 */
typedef struct DDLOptionDef
{
	const char *name;			/* Option name (case-insensitive) */
	uint32		flag;			/* Flag to set */
	bool		set_on_true;	/* If true, set flag when value is true; if
								 * false, set flag when value is false */
} DDLOptionDef;

/*
 * Array of supported DDL options.
 * To add a new option, simply add an entry to this array.
 */
static const DDLOptionDef ddl_option_defs[] = {
	{"pretty", PG_DDL_PRETTY_INDENT, true},
	{"defaults", PG_DDL_WITH_DEFAULTS, true},
	{"owner", PG_DDL_NO_OWNER, false},
	{"tablespace", PG_DDL_NO_TABLESPACE, false},
};

#define GET_DDL_PRETTY_FLAGS(pretty) \
	((pretty) ? (PRETTYFLAG_INDENT) \
	 : 0)

/* Local function declarations */
static char *pg_get_database_ddl_worker(Oid db_oid, uint32 ddl_flags);
static void get_formatted_string(StringInfo buf, int prettyFlags, int nSpaces, const char *fmt,...) pg_attribute_printf(4, 5);
static uint32 parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start);

/*
 * get_formatted_string
 *
 * Helper function to append formatted strings to a StringInfo buffer, with
 * optional pretty-printing based on flags.
 *
 * prettyFlags - Based on prettyFlags the output includes spaces and
 *               newlines (\n).
 * nSpaces - indent with specified number of space characters.
 * fmt - printf-style format string used by appendStringInfoVA.
 */
static void
get_formatted_string(StringInfo buf, int prettyFlags, int nSpaces, const char *fmt,...)
{
	va_list		args;

	if (prettyFlags & PRETTYFLAG_INDENT)
	{
		appendStringInfoChar(buf, '\n');
		/* Indent with spaces */
		appendStringInfoSpaces(buf, nSpaces);
	}
	else
		appendStringInfoChar(buf, ' ');

	for (;;)
	{
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(buf, needed);
	}
}

/*
 * parse_ddl_options - Generic helper to parse variadic name/value options
 * fcinfo: The FunctionCallInfo from the calling function
 * variadic_start: The argument position where variadic arguments start
 *
 * Returns: Bitmask of flags based on the parsed options.
 *
 * Options are passed as name/value pairs.
 * For example: pg_get_database_ddl('mydb', 'owner', false, 'pretty', true)
 */
static uint32
parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start)
{
	uint32		flags = 0;
	uint32		seen_flags = 0;
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;
	int			nargs;

	/* Extract variadic arguments */
	nargs = extract_variadic_args(fcinfo, variadic_start, true,
								  &args, &types, &nulls);

	/* If no options provided (VARIADIC NULL), return the empty bitmask */
	if (nargs <= 0)
		return flags;

	/*
	 * Handle the case where DEFAULT NULL was used and no explicit variadic
	 * arguments were provided. In this case, we get a single NULL argument.
	 */
	if (nargs == 1 && nulls[0])
		return flags;

	/* Arguments must come in name/value pairs */
	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("variadic arguments must be name/value pairs"),
				 errhint("Provide an even number of variadic arguments that can be divided into pairs.")));

	for (int i = 0; i < nargs; i += 2)
	{
		char	   *name;
		bool		bval;
		bool		found = false;

		/* Key must not be null */
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name at variadic position %d is null", i + 1)));

		/* Key must be text type */
		if (types[i] != TEXTOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name at variadic position %d has type %s, expected type %s",
							i + 1, format_type_be(types[i]),
							format_type_be(TEXTOID))));

		name = TextDatumGetCString(args[i]);

		/* Value must not be null */
		if (nulls[i + 1])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("value for option \"%s\" must not be null",
							name)));

		/* Value must be boolean or text type */
		if (types[i + 1] == BOOLOID)
		{
			bval = DatumGetBool(args[i + 1]);
		}
		else if (types[i + 1] == TEXTOID)
		{
			char	   *valstr = TextDatumGetCString(args[i + 1]);

			if (!parse_bool(valstr, &bval))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("value for option \"%s\" at position %d has invalid value \"%s\"",
								name, i + 2, valstr),
						 errhint("Valid values are: true, false, yes, no, 1, 0, on, off.")));
			pfree(valstr);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("value for option \"%s\" at position %d has type %s, expected type boolean or text",
							name, i + 2, format_type_be(types[i + 1]))));
		}

		/*
		 * Look up the option in the ddl_option_defs array and set the
		 * appropriate flag based on the value.
		 */
		for (int j = 0; j < lengthof(ddl_option_defs); j++)
		{
			const DDLOptionDef *opt = &ddl_option_defs[j];

			if (pg_strcasecmp(name, opt->name) == 0)
			{
				/* Error if this option was already specified */
				if (seen_flags & opt->flag)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("option \"%s\" is specified more than once", name)));

				seen_flags |= opt->flag;

				/*
				 * Set the flag if the value matches the set_on_true
				 * condition: if set_on_true is true, set flag when bval is
				 * true; if set_on_true is false, set flag when bval is false.
				 */
				if (bval == opt->set_on_true)
					flags |= opt->flag;

				found = true;
				break;
			}
		}

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized option: \"%s\"", name)));

		pfree(name);
	}

	return flags;
}

/*
 * pg_get_database_ddl
 *
 * Generate a CREATE DATABASE statement for the specified database oid.
 *
 * db_oid - OID of the database for which to generate the DDL.
 * options - Variadic name/value pairs to modify the output.
 */
Datum
pg_get_database_ddl(PG_FUNCTION_ARGS)
{
	Oid			db_oid;
	uint32		ddl_flags;
	char	   *res;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	db_oid = PG_GETARG_OID(0);

	/* Parse variadic options starting from argument 1 */
	ddl_flags = parse_ddl_options(fcinfo, 1);

	res = pg_get_database_ddl_worker(db_oid, ddl_flags);

	PG_RETURN_TEXT_P(cstring_to_text(res));
}

static char *
pg_get_database_ddl_worker(Oid db_oid, uint32 ddl_flags)
{
	const char *encoding;
	bool		attr_isnull;
	Datum		dbvalue;
	HeapTuple	tuple_database;
	Form_pg_database dbform;
	StringInfoData buf;
	StringInfoData optbuf;
	AclResult	aclresult;
	HeapTuple	tmpl_tuple;
	int			tmpl_encoding = -1;
	char	   *collate;
	char	   *ctype;

	/* Variables for ddl_options parsing */
	int			pretty_flags = 0;
	bool		is_with_defaults = false;

	/* Set the appropriate flags */
	if (ddl_flags & PG_DDL_PRETTY_INDENT)
		pretty_flags = GET_DDL_PRETTY_FLAGS(1);

	is_with_defaults = (ddl_flags & PG_DDL_WITH_DEFAULTS) != 0;

	/*
	 * User must have connect privilege for target database.
	 */
	aclresult = object_aclcheck(DatabaseRelationId, db_oid, GetUserId(),
								ACL_CONNECT);
	if (aclresult != ACLCHECK_OK &&
		!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(db_oid));
	}

	/* Look up the database in pg_database */
	tuple_database = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db_oid));
	if (!HeapTupleIsValid(tuple_database))
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("database with oid %u does not exist", db_oid));

	dbform = (Form_pg_database) GETSTRUCT(tuple_database);

	initStringInfo(&buf);
	initStringInfo(&optbuf);

	/*
	 * Build the options into a separate buffer first, so we can emit WITH
	 * only when there are options to show.
	 */

	/* Set the OWNER in the DDL if owner is not omitted */
	if (OidIsValid(dbform->datdba) && !(ddl_flags & PG_DDL_NO_OWNER))
	{
		char	   *dbowner = GetUserNameFromId(dbform->datdba, false);

		get_formatted_string(&optbuf, pretty_flags, 8, "OWNER = %s",
							 quote_identifier(dbowner));
	}

	/*
	 * Emit ENCODING if it differs from template1's encoding, or if defaults
	 * are requested.  The default encoding for CREATE DATABASE comes from the
	 * template database (template1), not a fixed value.
	 */
	encoding = pg_encoding_to_char(dbform->encoding);

	tmpl_tuple = SearchSysCache1(DATABASEOID,
								 ObjectIdGetDatum(Template1DbOid));
	if (HeapTupleIsValid(tmpl_tuple))
	{
		Form_pg_database tmplform = (Form_pg_database) GETSTRUCT(tmpl_tuple);

		tmpl_encoding = tmplform->encoding;
		ReleaseSysCache(tmpl_tuple);
	}

	if (is_with_defaults || dbform->encoding != tmpl_encoding)
		get_formatted_string(&optbuf, pretty_flags, 8, "ENCODING = %s",
							 quote_literal_cstr(encoding));

	/*
	 * LC_COLLATE and LC_CTYPE are BKI_FORCE_NOT_NULL, always present. Emit
	 * LOCALE when they match (like pg_dump), otherwise emit separately.
	 */
	dbvalue = SysCacheGetAttr(DATABASEOID, tuple_database,
								Anum_pg_database_datcollate, &attr_isnull);
	Assert(!attr_isnull);
	collate = TextDatumGetCString(dbvalue);

	dbvalue = SysCacheGetAttr(DATABASEOID, tuple_database,
								Anum_pg_database_datctype, &attr_isnull);
	Assert(!attr_isnull);
	ctype = TextDatumGetCString(dbvalue);

	if (strcmp(collate, ctype) == 0)
	{
		get_formatted_string(&optbuf, pretty_flags, 8, "LOCALE = %s",
								quote_literal_cstr(collate));
	}
	else
	{
		get_formatted_string(&optbuf, pretty_flags, 8, "LC_COLLATE = %s",
								quote_literal_cstr(collate));
		get_formatted_string(&optbuf, pretty_flags, 8, "LC_CTYPE = %s",
								quote_literal_cstr(ctype));
	}


	/*
	 * Fetch datlocale: emit as BUILTIN_LOCALE or ICU_LOCALE depending on the
	 * provider.  For libc, datlocale should always be NULL.
	 */
	dbvalue = SysCacheGetAttr(DATABASEOID, tuple_database,
							  Anum_pg_database_datlocale, &attr_isnull);
	if (!attr_isnull && dbform->datlocprovider == COLLPROVIDER_BUILTIN)
		get_formatted_string(&optbuf, pretty_flags, 8, "BUILTIN_LOCALE = %s",
							 quote_literal_cstr(TextDatumGetCString(dbvalue)));
	else if (!attr_isnull && dbform->datlocprovider == COLLPROVIDER_ICU)
		get_formatted_string(&optbuf, pretty_flags, 8, "ICU_LOCALE = %s",
							 quote_literal_cstr(TextDatumGetCString(dbvalue)));
	else
		Assert(attr_isnull);

	/* Fetch the value of ICU_RULES */
	dbvalue = SysCacheGetAttr(DATABASEOID, tuple_database,
							  Anum_pg_database_daticurules, &attr_isnull);
	if (!attr_isnull && dbform->datlocprovider == COLLPROVIDER_ICU)
		get_formatted_string(&optbuf, pretty_flags, 8, "ICU_RULES = %s",
							 quote_literal_cstr(TextDatumGetCString(dbvalue)));

	/*
	 * Emit COLLATION_VERSION only when defaults are requested.  Normally this
	 * is an internal implementation detail that should be determined freshly
	 * by the target cluster (similar to how pg_dump only emits it during
	 * binary upgrades).
	 */
	if (is_with_defaults)
	{
		dbvalue = SysCacheGetAttr(DATABASEOID, tuple_database,
								  Anum_pg_database_datcollversion, &attr_isnull);
		if (!attr_isnull)
			get_formatted_string(&optbuf, pretty_flags, 8, "COLLATION_VERSION = %s",
								 quote_literal_cstr(TextDatumGetCString(dbvalue)));
	}

	/* Set the appropriate LOCALE_PROVIDER */
	if (dbform->datlocprovider == COLLPROVIDER_BUILTIN)
		get_formatted_string(&optbuf, pretty_flags, 8, "LOCALE_PROVIDER = builtin");
	else if (dbform->datlocprovider == COLLPROVIDER_ICU)
		get_formatted_string(&optbuf, pretty_flags, 8, "LOCALE_PROVIDER = icu");
	else
		get_formatted_string(&optbuf, pretty_flags, 8, "LOCALE_PROVIDER = libc");

	/* Set the TABLESPACE in the DDL if tablespace is not omitted */
	if (OidIsValid(dbform->dattablespace) && !(ddl_flags & PG_DDL_NO_TABLESPACE))
	{
		if (is_with_defaults ||
			dbform->dattablespace != DEFAULTTABLESPACE_OID)
		{
			char	   *dbTablespace = get_tablespace_name(dbform->dattablespace);

			get_formatted_string(&optbuf, pretty_flags, 8, "TABLESPACE = %s",
								 quote_identifier(dbTablespace));
		}
	}

	if (is_with_defaults || !dbform->datallowconn)
	{
		get_formatted_string(&optbuf, pretty_flags, 8, "ALLOW_CONNECTIONS = %s",
							 dbform->datallowconn ? "true" : "false");
	}

	if (is_with_defaults ||
		dbform->datconnlimit != DATCONNLIMIT_UNLIMITED)
	{
		get_formatted_string(&optbuf, pretty_flags, 8, "CONNECTION LIMIT = %d",
							 dbform->datconnlimit);
	}

	if (is_with_defaults || dbform->datistemplate)
		get_formatted_string(&optbuf, pretty_flags, 8, "IS_TEMPLATE = %s",
							 dbform->datistemplate ? "true" : "false");

	/* Build the CREATE DATABASE statement */
	appendStringInfo(&buf, "CREATE DATABASE %s",
					 quote_identifier(dbform->datname.data));

	/* Only emit WITH if there are options */
	if (optbuf.len > 0)
	{
		get_formatted_string(&buf, pretty_flags, 4, "WITH");
		appendStringInfoString(&buf, optbuf.data);
	}

	pfree(optbuf.data);
	appendStringInfoChar(&buf, ';');

	ReleaseSysCache(tuple_database);

	return buf.data;
}
