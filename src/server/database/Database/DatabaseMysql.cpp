/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef DO_POSTGRESQL

#include <errmsg.h>
#include <mysqld_error.h>
#include "Util.h"
//#include "Policies/SingletonImp.h"
#include "Platform/Define.h"
#include "Threading.h"
#include "DatabaseEnv.h"
#include "Timer.h"
#if PLATFORM == PLATFORM_UNIX
#include <sys/file.h>
#endif


size_t DatabaseMysql::db_count = 0;

void DatabaseMysql::ThreadStart()
{
    mysql_thread_init();
}

void DatabaseMysql::ThreadEnd()
{
    mysql_thread_end();
}

DatabaseMysql::DatabaseMysql()
{
    // before first connection
    if (db_count++ == 0)
    {
        // Mysql Library Init
        mysql_library_init(-1, nullptr, nullptr);

        if (!mysql_thread_safe())
        {
            sLog.outError("FATAL ERROR: Used MySQL library isn't thread-safe.");
            Log::WaitBeforeContinueIfNeed();
            exit(1);
        }
    }
}

DatabaseMysql::~DatabaseMysql()
{
    StopServer();

    //Free Mysql library pointers for last ~DB
    if(--db_count == 0)
        mysql_library_end();
}

SqlConnection* DatabaseMysql::CreateConnection()
{
    return new MySQLConnection(*this);
}

MySQLConnection::~MySQLConnection()
{
    FreePreparedStatements();
    mysql_close(mMysql);
}

bool MySQLConnection::ExecuteFile(const char* file)
{
    if (!mMysql)
        return false;

    if (mysql_set_server_option(mMysql, MYSQL_OPTION_MULTI_STATEMENTS_ON))
    {
        sLog.outErrorDb("Cannot turn multi-statements on: %s", mysql_error(mMysql));
        return false;
    }

    mysql_autocommit(mMysql, 0);
    if (mysql_real_query(mMysql, "START TRANSACTION", sizeof("START TRANSACTION") - 1))
    {
        sLog.outErrorDb("Couldn't start transaction for db update file: %s", file);
        return false;
    }

    bool in_transaction = true;
    bool success = false;

    if (FILE* fp = ACE_OS::fopen(file, "rb"))
    {
#if PLATFORM == PLATFORM_UNIX
        flock(fileno(fp), LOCK_SH);
#endif
        //------

        struct stat info;
        fstat(fileno(fp), &info);

        // if less than 1MB allocate on stack, else on heap
        char* contents = (info.st_size > 1024 * 1024) ? new char[info.st_size] : (char*)alloca(info.st_size);

        if (ACE_OS::fread(contents, info.st_size, 1, fp) == 1)
        {
            if (mysql_real_query(mMysql, contents, info.st_size))
            {
                sLog.outErrorDb("Cannot execute file %s, size: %lu: %s", file, info.st_size, mysql_error(mMysql));
            }
            else
            {
                do
                {
                    if (mysql_field_count(mMysql))
                        if (MYSQL_RES* result = mysql_use_result(mMysql))
                            mysql_free_result(result);
                } while (0 == mysql_next_result(mMysql));

                // check whether the last mysql_next_result ended with an error
                if (*mysql_error(mMysql))
                {
                    success = false;
                    sLog.outErrorDb("Cannot execute file %s, size: %lu: %s", file, info.st_size, mysql_error(mMysql));
                    if (mysql_rollback(mMysql))
                        sLog.outErrorDb("ExecuteFile(): Rollback ended with an error!");
                    else
                        in_transaction = false;
                }
                else
                {
                    if (mysql_commit(mMysql))
                        sLog.outErrorDb("mysql_commit() failed. Update %s will not be applied!", file);
                    else
                        in_transaction = false;
                    success = true;
                }
            }
        }
        else
        {
            sLog.outErrorDb("Couldn't read file %s, size: %lu", file, info.st_size);
            return false;
        }

        // if allocated on heap, free memory
        if (info.st_size > 1024 * 1024)
            delete[] contents;

        //------
#if PLATFORM == PLATFORM_UNIX
        flock(fileno(fp), LOCK_UN);
#endif
        ACE_OS::fclose(fp);
    }

    mysql_set_server_option(mMysql, MYSQL_OPTION_MULTI_STATEMENTS_OFF);
    mysql_autocommit(mMysql, 1);
    if (in_transaction)
        mysql_rollback(mMysql);

    sLog.outErrorDb("Successfully applied file %s", file);

    return success;
}

bool MySQLConnection::OpenConnection(bool reconnect)
{
    MYSQL* mysqlInit = mysql_init(nullptr);
    if (!mysqlInit)
    {
        sLog.outError("Could not initialize Mysql connection");
        return false;
    }

    mysql_options(mysqlInit, MYSQL_SET_CHARSET_NAME, "utf8");

    if (m_use_socket)                                           // socket use option (Unix/Linux)
    {
        unsigned int opt = MYSQL_PROTOCOL_SOCKET;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
    }

    mMysql = mysql_real_connect(mysqlInit, m_host.c_str(), m_user.c_str(),
        m_password.c_str(), m_database.c_str(), m_port, nullptr, 0);

    if (mMysql)
    {
        sLog.outErrorDb("Connected to MySQL database at %s",
            m_host.c_str());
        sLog.outErrorDb("MySQL client library: %s", mysql_get_client_info());
        sLog.outErrorDb("MySQL server ver: %s ", mysql_get_server_info(mMysql));

        /*----------SET AUTOCOMMIT ON---------*/
        // It seems mysql 5.0.x have enabled this feature
        // by default. In crash case you can lose data!!!
        // So better to turn this off
        // ---
        // This is wrong since mangos use transactions,
        // autocommit is turned of during it.
        // Setting it to on makes atomic updates work
        // ---
        // LEAVE 'AUTOCOMMIT' MODE ALWAYS ENABLED!!!
        // W/O IT EVEN 'SELECT' QUERIES WOULD REQUIRE TO BE WRAPPED INTO 'START TRANSACTION'<>'COMMIT' CLAUSES!!!
        if (!mysql_autocommit(mMysql, 1))
            sLog.outErrorDb("AUTOCOMMIT SUCCESSFULLY SET TO 1");
        else
            sLog.outErrorDb("AUTOCOMMIT NOT SET TO 1");
        /*-------------------------------------*/

        // set connection properties to UTF8 to properly handle locales for different
        // server configs - core sends data in UTF8, so MySQL must expect UTF8 too
        Execute("SET NAMES `utf8`");
        Execute("SET CHARACTER SET `utf8`");

        return true;
    }
    else
    {
        sLog.outError("Could not connect to MySQL database at %s: %s\n",
            m_host.c_str(), mysql_error(mysqlInit));
        mysql_close(mysqlInit);
        return false;
    }
}

bool MySQLConnection::Reconnect()
{
    sLog.outString("Reconnection attempt to database %s (on %s)", m_database.c_str(), m_host.c_str());

    if (OpenConnection(true))
    {
        FreePreparedStatements(); // We need to prepare everything again!
        sLog.outString("Successfully reconnected to %s @%s:%u.",
            m_database.c_str(), m_host.c_str(), m_port);

        return true;
    }

    return false; // Failed to reconnect
}

bool MySQLConnection::HandleMySQLError(uint32 errNo)
{
    switch (errNo)
    {
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
            #if !(MARIADB_VERSION_ID >= 100200)
        case CR_INVALID_CONN_HANDLE:
            #endif
        case CR_SERVER_LOST_EXTENDED:
        {
            mysql_close(mMysql);
            return Reconnect();
        }

        case ER_LOCK_DEADLOCK:
            return false;
        // Query related errors - skip query
        case ER_WRONG_VALUE_COUNT:
        case ER_DUP_ENTRY:
            return false;

        // Outdated table or database structure - terminate core
        case ER_BAD_FIELD_ERROR:
        case ER_NO_SUCH_TABLE:
            sLog.outErrorDb("Your database structure is not up to date. Please make sure you have executed all the queries in the sql/updates folders.");
            ASSERT(false);
            return false;
        case ER_PARSE_ERROR:
            sLog.outErrorDb("Error while parsing SQL. Core fix required.");
            ASSERT(false);
            return false;
        default:
            sLog.outErrorDb("Unhandled MySQL errno %u. Unexpected behaviour possible.", errNo);
            return false;
    }
}

bool MySQLConnection::_Query(char const* sql, MYSQL_RES** pResult, MYSQL_FIELD** pFields, uint64* pRowCount, uint32* pFieldCount)
{
    if (!mMysql && !Reconnect())
    {
        return false;
    }
        

    uint32 _s = getMSTime();

    if (mysql_query(mMysql, sql))
    {
        uint32 lErrno = mysql_errno(mMysql);

        sLog.outErrorDb("SQL: %s", sql);
        sLog.outErrorDb("[%u] %s", lErrno, mysql_error(mMysql));

        if (HandleMySQLError(lErrno)) // If error is handled, just try again
            return _Query(sql, pResult, pFields, pRowCount, pFieldCount);

        return false;
    }
    else
    {
        sLog.outErrorDb("[%u ms] SQL: %s", getMSTimeDiff(_s,getMSTime()), sql);
    }

    *pResult = mysql_store_result(mMysql);
    *pRowCount = mysql_affected_rows(mMysql);
    *pFieldCount = mysql_field_count(mMysql);

    if (!*pResult)
        return false;

    if (!*pRowCount)
    {
        mysql_free_result(*pResult);
        return false;
    }

    *pFields = mysql_fetch_fields(*pResult);
    return true;
}

bool DatabaseMysql::_Query(char const* sql, MYSQL_RES** pResult, MYSQL_FIELD** pFields, uint64* pRowCount, uint32* pFieldCount)
{

    if (!mMysql)
    {
        return false;
    }


    uint32 _s = getMSTime();

    if (mysql_query(mMysql, sql))
    {
        uint32 lErrno = mysql_errno(mMysql);

        sLog.outErrorDb("SQL: %s", sql);
        sLog.outErrorDb("[%u] %s", lErrno, mysql_error(mMysql));

        //if (HandleMySQLError(lErrno)) // If error is handled, just try again
        //    return _Query(sql, pResult, pFields, pRowCount, pFieldCount);

        return false;
    }
    else
    {
        sLog.outErrorDb("[%u ms] SQL: %s", getMSTimeDiff(_s, getMSTime()), sql);
    }

    *pResult = mysql_store_result(mMysql);
    *pRowCount = mysql_affected_rows(mMysql);
    *pFieldCount = mysql_field_count(mMysql);

    if (!*pResult)
        return false;

    if (!*pRowCount)
    {
        mysql_free_result(*pResult);
        return false;
    }

    *pFields = mysql_fetch_fields(*pResult);
    return true;
}

QueryResult* MySQLConnection::Query(char const* sql)
{
    MYSQL_RES* result = nullptr;
    MYSQL_FIELD* fields = nullptr;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if(!_Query(sql,&result,&fields,&rowCount,&fieldCount))
        return nullptr;

    QueryResultMysql *queryResult = new QueryResultMysql(result, fields, rowCount, fieldCount);

    queryResult->NextRow();
    return queryResult;
}

QueryNamedResult* MySQLConnection::QueryNamed(char const* sql)
{
    MYSQL_RES* result = nullptr;
    MYSQL_FIELD* fields = nullptr;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if(!_Query(sql,&result,&fields,&rowCount,&fieldCount))
        return nullptr;

    QueryFieldNames names(fieldCount);
    for (uint32 i = 0; i < fieldCount; i++)
        names[i] = fields[i].name;

    QueryResultMysql *queryResult = new QueryResultMysql(result, fields, rowCount, fieldCount);

    queryResult->NextRow();
    return new QueryNamedResult(queryResult,names);
}

bool MySQLConnection::Execute(char const* sql)
{
    if (!mMysql)
        return false;

    uint32 _s = getMSTime();

    if (mysql_query(mMysql, sql))
    {
        uint32 lErrno = mysql_errno(mMysql);

        sLog.outErrorDb("SQL: %s", sql);
        sLog.outErrorDb("[%u] %s", lErrno, mysql_error(mMysql));

        if (HandleMySQLError(lErrno)) // If error is handled, just try again
            return Execute(sql);
        return false;
    }
    else
    {
        sLog.outErrorDb("[%u ms] SQL: %s", getMSTimeDiff(_s,getMSTime()), sql);
    }

    return true;
}

bool MySQLConnection::_TransactionCmd(char const* sql)
{
    if (mysql_query(mMysql, sql))
    {
        sLog.outError("SQL: %s", sql);
        sLog.outError("SQL ERROR: %s", mysql_error(mMysql));
        return false;
    }
    else
    {
        sLog.outErrorDb("SQL: %s", sql);
    }
    return true;
}

bool MySQLConnection::BeginTransaction()
{
    return _TransactionCmd("START TRANSACTION");
}

bool MySQLConnection::CommitTransaction()
{
    return _TransactionCmd("COMMIT");
}

bool MySQLConnection::RollbackTransaction()
{
    return _TransactionCmd("ROLLBACK");
}

unsigned long MySQLConnection::escape_string(char* to, char const* from, unsigned long length)
{
    if (!mMysql || !to || !from || !length)
        return 0;

    return mysql_real_escape_string(mMysql, to, from, length);
}

//////////////////////////////////////////////////////////////////////////
SqlPreparedStatement* MySQLConnection::CreateStatement(std::string const& fmt)
{
    return new MySqlPreparedStatement(fmt, *this, mMysql);
}


//////////////////////////////////////////////////////////////////////////
MySqlPreparedStatement::MySqlPreparedStatement(std::string const& fmt, SqlConnection& conn, MYSQL* mysql) : SqlPreparedStatement(fmt, conn),
    m_pMySQLConn(mysql), m_stmt(nullptr), m_pInputArgs(nullptr), m_pResult(nullptr), m_pResultMetadata(nullptr)
{
}

MySqlPreparedStatement::~MySqlPreparedStatement()
{
    RemoveBinds();
}

bool MySqlPreparedStatement::prepare()
{
    if(isPrepared())
        return true;

    //remove old binds
    RemoveBinds();

    //create statement object
    m_stmt = mysql_stmt_init(m_pMySQLConn);
    if (!m_stmt)
    {
        sLog.outError("SQL: mysql_stmt_init()() failed ");
        return false;
    }

    //prepare statement
    if (mysql_stmt_prepare(m_stmt, m_szFmt.c_str(), m_szFmt.length()))
    {
        sLog.outError("SQL: mysql_stmt_prepare() failed for '%s'", m_szFmt.c_str());
        sLog.outError("SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    /* Get the parameter count from the statement */
    m_nParams = mysql_stmt_param_count(m_stmt);

    /* Fetch result set meta information */
    m_pResultMetadata = mysql_stmt_result_metadata(m_stmt);
    //if we do not have result metadata
    if (!m_pResultMetadata && strnicmp(m_szFmt.c_str(), "select", 6) == 0)
    {
        sLog.outError("SQL: no meta information for '%s'", m_szFmt.c_str());
        sLog.outError("SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    //bind input buffers
    if(m_nParams)
    {
        m_pInputArgs = new MYSQL_BIND[m_nParams];
        memset(m_pInputArgs, 0, sizeof(MYSQL_BIND) * m_nParams);
    }

    //check if we have a statement which returns result sets
    if(m_pResultMetadata)
    {
        //our statement is query
        m_bIsQuery = true;
        /* Get total columns in the query */
        m_nColumns = mysql_num_fields(m_pResultMetadata);

        //bind output buffers
    }

    m_bPrepared = true;
    return true;
}

void MySqlPreparedStatement::bind(SqlStmtParameters const& holder)
{
    if(!isPrepared())
    {
        ASSERT(false);
        return;
    }

    //finalize adding params
    if(!m_pInputArgs)
        return;

    //verify if we bound all needed input parameters
    if(m_nParams != holder.boundParams())
    {
        ASSERT(false);
        return;
    }

    int nIndex = 0;
    SqlStmtParameters::ParameterContainer const& _args = holder.params();

    SqlStmtParameters::ParameterContainer::const_iterator iter_last = _args.end();
    for (SqlStmtParameters::ParameterContainer::const_iterator iter = _args.begin(); iter != iter_last; ++iter)
    {
        //bind parameter
        addParam(nIndex++, (*iter));
    }

    //bind input arguments
    if (mysql_stmt_bind_param(m_stmt, m_pInputArgs))
    {
        sLog.outError("SQL ERROR: mysql_stmt_bind_param() failed\n");
        sLog.outError("SQL ERROR: %s", mysql_stmt_error(m_stmt));
    }
}

void MySqlPreparedStatement::addParam(int nIndex, SqlStmtFieldData const& data)
{
    ASSERT(m_pInputArgs);
    ASSERT(nIndex < static_cast <int32> (m_nParams));

    MYSQL_BIND& pData = m_pInputArgs[nIndex];

    my_bool bUnsigned = 0;
    enum_field_types dataType = ToMySQLType(data, bUnsigned);

    //setup MYSQL_BIND structure
    pData.buffer_type = dataType;
    pData.is_unsigned = bUnsigned;
    pData.buffer = data.buff();
    pData.length = 0;
    pData.buffer_length = data.type() == FIELD_STRING ? data.size() : 0;
}

void MySqlPreparedStatement::RemoveBinds()
{
    if(!m_stmt)
        return;

    delete [] m_pInputArgs;
    delete [] m_pResult;

    mysql_free_result(m_pResultMetadata);
    mysql_stmt_close(m_stmt);

    m_stmt = nullptr;
    m_pResultMetadata = nullptr;
    m_pResult = nullptr;
    m_pInputArgs = nullptr;

    m_bPrepared = false;
}

bool MySqlPreparedStatement::execute()
{
    if(!isPrepared())
        return false;

    if(mysql_stmt_execute(m_stmt))
    {
        sLog.outError("SQL: cannot execute '%s'", m_szFmt.c_str());
        sLog.outError("SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    return true;
}

enum_field_types MySqlPreparedStatement::ToMySQLType(SqlStmtFieldData const& data, my_bool& bUnsigned)
{
    bUnsigned = 0;
    enum_field_types dataType = MYSQL_TYPE_NULL;

    switch (data.type())
    {
    case FIELD_NONE:    dataType = MYSQL_TYPE_NULL;                     break;
    case FIELD_BOOL:    dataType = MYSQL_TYPE_BIT;      bUnsigned = 1;  break;
    case FIELD_I8:      dataType = MYSQL_TYPE_TINY;                     break;
    case FIELD_UI8:     dataType = MYSQL_TYPE_TINY;     bUnsigned = 1;  break;
    case FIELD_I16:     dataType = MYSQL_TYPE_SHORT;                    break;
    case FIELD_UI16:    dataType = MYSQL_TYPE_SHORT;    bUnsigned = 1;  break;
    case FIELD_I32:     dataType = MYSQL_TYPE_LONG;                     break;
    case FIELD_UI32:    dataType = MYSQL_TYPE_LONG;     bUnsigned = 1;  break;
    case FIELD_I64:     dataType = MYSQL_TYPE_LONGLONG;                 break;
    case FIELD_UI64:    dataType = MYSQL_TYPE_LONGLONG; bUnsigned = 1;  break;
    case FIELD_FLOAT:   dataType = MYSQL_TYPE_FLOAT;                    break;
    case FIELD_DOUBLE:  dataType = MYSQL_TYPE_DOUBLE;                   break;
    case FIELD_STRING:  dataType = MYSQL_TYPE_STRING;                   break;
    }

    return dataType;
}
#endif