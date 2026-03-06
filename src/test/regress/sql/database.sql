--
-- Reconstruct DDL
--
-- To produce stable regression test output, it's usually necessary to
-- ignore collation and locale related details. This filter
-- functions removes collation and locale related details.

CREATE OR REPLACE FUNCTION ddl_filter(ddl_input TEXT)
RETURNS TEXT AS $$
DECLARE
    cleaned_ddl TEXT;
BEGIN
    -- Remove %LOCALE_PROVIDER% placeholders
    cleaned_ddl := regexp_replace(
        ddl_input,
        '\s*\mLOCALE_PROVIDER\M\s*=\s*([''"]?[^''"\s]+[''"]?)',
        '',
        'gi'
    );

    -- Remove LC_COLLATE assignments
    cleaned_ddl := regexp_replace(
        cleaned_ddl,
        '\s*LC_COLLATE\s*=\s*([''"])[^''"]*\1',
        '',
        'gi'
    );

    -- Remove LC_CTYPE assignments
    cleaned_ddl := regexp_replace(
        cleaned_ddl,
        '\s*LC_CTYPE\s*=\s*([''"])[^''"]*\1',
        '',
        'gi'
    );

    -- Remove %LOCALE% placeholders
    cleaned_ddl := regexp_replace(
        cleaned_ddl,
        '\s*\S*LOCALE\S*\s*=?\s*([''"])[^''"]*\1',
        '',
        'gi'
    );

    -- Remove %COLLATION% placeholders
    cleaned_ddl := regexp_replace(
        cleaned_ddl,
        '\s*\S*COLLATION\S*\s*=?\s*([''"])[^''"]*\1',
        '',
        'gi'
    );

    -- Remove ENCODING assignments
    cleaned_ddl := regexp_replace(
        cleaned_ddl,
        '\s*ENCODING\s*=\s*([''"])[^''"]*\1',
        '',
        'gi'
    );

    RETURN cleaned_ddl;
END;
$$ LANGUAGE plpgsql;

CREATE DATABASE regression_tbd
	ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;
ALTER DATABASE regression_tbd RENAME TO regression_utf8;
ALTER DATABASE regression_utf8 SET TABLESPACE regress_tblspace;
ALTER DATABASE regression_utf8 SET TABLESPACE pg_default;
ALTER DATABASE regression_utf8 CONNECTION_LIMIT 123;

-- Test PgDatabaseToastTable.  Doing this with GRANT would be slow.
BEGIN;
UPDATE pg_database
SET datacl = array_fill(makeaclitem(10, 10, 'USAGE', false), ARRAY[5e5::int])
WHERE datname = 'regression_utf8';
-- load catcache entry, if nothing else does
ALTER DATABASE regression_utf8 RENAME TO regression_rename_rolled_back;
ROLLBACK;

CREATE ROLE regress_datdba_before;
CREATE ROLE regress_datdba_after;
ALTER DATABASE regression_utf8 OWNER TO regress_datdba_before;
REASSIGN OWNED BY regress_datdba_before TO regress_datdba_after;

-- Test pg_get_database_ddl
-- Database doesn't exists
SELECT pg_get_database_ddl('regression_database');

-- Test NULL value
SELECT pg_get_database_ddl(NULL);

-- Without options
SELECT ddl_filter(pg_get_database_ddl('regression_utf8'));

-- With No Owner
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', false));

-- With No Tablespace
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'defaults', true, 'tablespace', false));

-- With Defaults
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'defaults', true));

-- With No Owner, No Tablespace and With Defaults
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', false, 'tablespace', false, 'defaults', true));

-- With Pretty formatted
\pset format unaligned
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'pretty', true));

-- With No Owner and No Tablespace
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'pretty', true, 'owner', false, 'tablespace', false));

-- With Defaults
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'pretty', true, 'defaults', true));

-- With No Owner, No Tablespace and With Defaults
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'pretty', true, 'owner', false, 'tablespace', false, 'defaults', true));

-- Test with text values: 'yes', 'no', '1', '0', 'on', 'off'
\pset format aligned
-- Using 'yes' and 'no'
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', 'no', 'defaults', 'yes'));

-- Using '1' and '0'
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', '0', 'tablespace', '0', 'defaults', '1'));

-- Using 'on' and 'off'
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', 'off', 'pretty', 'on'));

-- Mixed boolean and text values
SELECT ddl_filter(pg_get_database_ddl('regression_utf8', 'owner', false, 'defaults', 'true', 'tablespace', 'no'));

-- Test duplicate option (should error)
SELECT pg_get_database_ddl('regression_utf8', 'owner', false, 'owner', true);

-- Test invalid text value (should error)
SELECT pg_get_database_ddl('regression_utf8', 'owner', 'invalid');

DROP DATABASE regression_utf8;
DROP FUNCTION ddl_filter(text);
DROP ROLE regress_datdba_before;
DROP ROLE regress_datdba_after;
