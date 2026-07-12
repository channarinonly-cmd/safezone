// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "auth.hpp"

#include <string.h>

#include <common/showmsg.hpp>
#include <common/sql.hpp>

#include "http.hpp"
#include "sqllock.hpp"
#include "web.hpp"


bool isAuthorized(const Request &request, bool checkGuildLeader) {
	if (!request.has_file("AuthToken"))
		return false;

	if (checkGuildLeader && !request.has_file("GDID"))
		return false;
	
	auto token_str = request.get_file_value("AuthToken").content;
	auto token = token_str.c_str();

	int account_id = 0;
	if (request.has_file("AID") && !request.get_file_value("AID").content.empty() && request.get_file_value("AID").content != "0") {
		account_id = std::stoi(request.get_file_value("AID").content);
	} else if (request.has_file("GID") && !request.get_file_value("GID").content.empty()) {
		int char_id = std::stoi(request.get_file_value("GID").content);
		SQLLock sl(CHAR_SQL_LOCK);
		sl.lock();
		Sql* handle = sl.getHandle();
		SqlStmt* stmt = SqlStmt_Malloc(handle);
		if (SQL_SUCCESS == SqlStmt_Prepare(stmt, "SELECT `account_id` FROM `%s` WHERE `char_id` = ? LIMIT 1", char_db_table)
			&& SQL_SUCCESS == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, sizeof(char_id))
			&& SQL_SUCCESS == SqlStmt_Execute(stmt)
			&& SQL_SUCCESS == SqlStmt_NextRow(stmt)
		) {
			int temp_aid = 0;
			SqlStmt_BindColumn(stmt, 0, SQLDT_INT, &temp_aid, 0, nullptr, nullptr);
			account_id = temp_aid;
		}
		SqlStmt_Free(stmt);
		sl.unlock();
	}

	if (account_id <= 0)
		return false;

	SQLLock loginlock(LOGIN_SQL_LOCK);

	loginlock.lock();

	auto handle = loginlock.getHandle();

	SqlStmt * stmt = SqlStmt_Malloc(handle);

	if (SQL_SUCCESS != SqlStmt_Prepare(stmt,
			"SELECT `account_id` FROM `%s` WHERE (`account_id` = ? AND `web_auth_token` = ? AND `web_auth_token_enabled` = '1')",
			login_table)
		|| SQL_SUCCESS != SqlStmt_BindParam(stmt, 0, SQLDT_INT, &account_id, sizeof(account_id))
		|| SQL_SUCCESS != SqlStmt_BindParam(stmt, 1, SQLDT_STRING, (void *)token, strlen(token))
		|| SQL_SUCCESS != SqlStmt_Execute(stmt)
	) {
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		loginlock.unlock();
		return false;
	}

	if (SqlStmt_NumRows(stmt) <= 0) {
		ShowDebug("Request with AID %d and token %s unverified\n", account_id, token);
		SqlStmt_Free(stmt);
		loginlock.unlock();
		return false;
	}

	SqlStmt_Free(stmt);
	loginlock.unlock();
	if (!checkGuildLeader) {
		// we're done, auth ok
		return true;
	}

	auto guild_id = std::stoi(request.get_file_value("GDID").content);

	SQLLock charlock(CHAR_SQL_LOCK);
	charlock.lock();
	handle = charlock.getHandle();
	stmt = SqlStmt_Malloc(handle);

	if (SQL_SUCCESS != SqlStmt_Prepare(stmt,
		"SELECT `account_id` FROM `%s` LEFT JOIN `%s` using (`char_id`) WHERE (`%s`.`account_id` = ? AND `%s`.`guild_id` = ?) LIMIT 1",
		guild_db_table, char_db_table, char_db_table, guild_db_table)
		|| SQL_SUCCESS != SqlStmt_BindParam(stmt, 0, SQLDT_INT, &account_id, sizeof(account_id))
		|| SQL_SUCCESS != SqlStmt_BindParam(stmt, 1, SQLDT_INT, &guild_id, sizeof(guild_id))
		|| SQL_SUCCESS != SqlStmt_Execute(stmt)
	) {
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		charlock.unlock();
		return false;
	}

	if (SqlStmt_NumRows(stmt) <= 0) {
		ShowDebug("Request with AID %d GDID %d and token %s unverified\n", account_id, guild_id, token);
		SqlStmt_Free(stmt);
		charlock.unlock();
		return false;
	}
	SqlStmt_Free(stmt);
	charlock.unlock();
	return true;
}

