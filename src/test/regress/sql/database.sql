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

DROP DATABASE regression_utf8;
DROP ROLE regress_datdba_before;
DROP ROLE regress_datdba_after;

-- Without Pretty formatted
-- Create a specific role to test
CREATE ROLE regress_ddl_database WITH SUPERUSER;
CREATE DATABASE "test_get_database_ddl"
    OWNER regress_ddl_database ENCODING 'UTF8' LC_COLLATE "C" LC_CTYPE "C"
    TEMPLATE template0;
SELECT pg_get_database_ddl('test_get_database_ddl', false);
DROP DATABASE "test_get_database_ddl";

-- Test LOCAL_PROVIDER and BUILTIN_LOCALE for builtin type
CREATE DATABASE "test_get_database_ddl_builtin"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    BUILTIN_LOCALE 'C.UTF-8' LOCALE_PROVIDER 'builtin';
SELECT pg_get_database_ddl('test_get_database_ddl_builtin', false);
DROP DATABASE "test_get_database_ddl_builtin";

-- Test ALLOW_CONNECTION and CONNECTION_LIMIT
CREATE DATABASE "test_get_database_ddl_conn"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    ALLOW_CONNECTIONS 0 CONNECTION LIMIT 50;
SELECT pg_get_database_ddl('test_get_database_ddl_conn', false);
DROP DATABASE "test_get_database_ddl_conn";

-- Database doesn't exists
SELECT pg_get_database_ddl('test_database', false);

-- With Pretty formatted
\pset format unaligned
CREATE DATABASE "test_get_database_ddl"
    OWNER regress_ddl_database ENCODING 'UTF8' LC_COLLATE "C" LC_CTYPE "C"
    TEMPLATE template0;
SELECT pg_get_database_ddl('test_get_database_ddl', true);
DROP DATABASE "test_get_database_ddl";

-- Test LOCAL_PROVIDER and BUILTIN_LOCALE for builtin type
CREATE DATABASE "test_get_database_ddl_builtin"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    BUILTIN_LOCALE 'C.UTF-8' LOCALE_PROVIDER 'builtin';
SELECT pg_get_database_ddl('test_get_database_ddl_builtin', true);
DROP DATABASE "test_get_database_ddl_builtin";

-- Test ALLOW_CONNECTION and CONNECTION_LIMIT
CREATE DATABASE "test_get_database_ddl_conn"
    OWNER regress_ddl_database TEMPLATE template0 ENCODING 'UTF8'
    LC_COLLATE "C" LC_CTYPE "C"
    ALLOW_CONNECTIONS 0 CONNECTION LIMIT 50;
SELECT pg_get_database_ddl('test_get_database_ddl_conn', true);
DROP DATABASE "test_get_database_ddl_conn";

-- Clean up
DROP ROLE regress_ddl_database;