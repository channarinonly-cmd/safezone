// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "autoattack_controller.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <common/showmsg.hpp>
#include <common/sql.hpp>

#include "auth.hpp"
#include "sqllock.hpp"
#include "web.hpp"

static bool autoattack_check_character(int account_id, int char_id)
{
	SQLLock sl(CHAR_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();
	SqlStmt* stmt = SqlStmt_Malloc(handle);
	bool ok = false;

	if (SQL_SUCCESS == SqlStmt_Prepare(stmt,
		"SELECT `char_id` FROM `%s` WHERE `account_id` = ? AND `char_id` = ? LIMIT 1",
		char_db_table)
		&& SQL_SUCCESS == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &account_id, sizeof(account_id))
		&& SQL_SUCCESS == SqlStmt_BindParam(stmt, 1, SQLDT_INT, &char_id, sizeof(char_id))
		&& SQL_SUCCESS == SqlStmt_Execute(stmt)
	) {
		ok = SqlStmt_NumRows(stmt) > 0;
	} else {
		SqlStmt_ShowDebug(stmt);
	}

	SqlStmt_Free(stmt);
	sl.unlock();
	return ok;
}

static std::unordered_set<int> autoattack_read_buff_item_ids()
{
	std::unordered_set<int> ids;
	std::ifstream file("db/custom/ai_item_buff.txt");
	std::string line;

	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '/')
			continue;

		std::stringstream ss(line);
		std::string field;
		if (!std::getline(ss, field, ','))
			continue;

		try {
			int item_id = std::stoi(field);
			if (item_id > 0)
				ids.insert(item_id);
		} catch (...) {
		}
	}

	return ids;
}

static bool autoattack_require_auth(const Request& req, Response& res, int& account_id, int& char_id)
{
	if (!req.has_file("AID") || !req.has_file("GID") || !isAuthorized(req, false)) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ไม่ได้รับอนุญาต", "text/plain; charset=utf-8");
		return false;
	}

	account_id = std::stoi(req.get_file_value("AID").content);
	char_id = std::stoi(req.get_file_value("GID").content);

	if (!autoattack_check_character(account_id, char_id)) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ตัวละครนี้ไม่ตรงกับบัญชี", "text/plain; charset=utf-8");
		return false;
	}

	return true;
}

HANDLER_FUNC(autoattack_page)
{
	static const char* html = R"HTML(
<!doctype html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ตั้งค่า AI ฟาร์ม</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:Tahoma,Arial,sans-serif;background:#f4f6f8;color:#20242a}
header{background:#1f2937;color:#fff;padding:18px 20px}main{max-width:1120px;margin:0 auto;padding:18px}
h1{font-size:22px;margin:0}h2{font-size:17px;margin:0 0 10px}.grid{display:grid;grid-template-columns:1fr;gap:14px}
@media(min-width:860px){.grid{grid-template-columns:1fr 1fr 1fr}.auth{grid-template-columns:1fr 1fr 2fr auto}}
.panel{background:#fff;border:1px solid #d9dee7;border-radius:8px;padding:14px}.auth{display:grid;gap:10px;margin-bottom:14px}
label{display:block;font-size:13px;color:#4b5563;margin-bottom:5px}input,textarea,button{font:inherit}
input,textarea{width:100%;border:1px solid #cbd5e1;border-radius:6px;padding:9px;background:#fff}
textarea{min-height:180px;resize:vertical}.hint{font-size:12px;color:#687386;margin-top:6px;line-height:1.45}
button{border:0;border-radius:6px;padding:10px 14px;background:#2563eb;color:#fff;cursor:pointer}
button.secondary{background:#475569}button:disabled{opacity:.55;cursor:not-allowed}.bar{display:flex;gap:8px;align-items:end}
.status{margin:10px 0 0;color:#0f766e;font-size:13px}.err{color:#b91c1c}
</style>
</head>
<body>
<header><h1>ตั้งค่า AI ฟาร์ม</h1></header>
<main>
	<section class="panel">
		<div class="auth">
			<div><label>AID</label><input id="aid" inputmode="numeric"></div>
			<div><label>GID / Char ID</label><input id="gid" inputmode="numeric"></div>
			<div><label>AuthToken</label><input id="token"></div>
			<div class="bar"><button id="load">โหลดค่า</button><button id="save" class="secondary">บันทึก</button></div>
		</div>
		<div class="hint">ใส่ข้อมูลตัวละครแล้วกดโหลดค่า รายการหนึ่งบรรทัดต่อหนึ่งรายการ</div>
		<div id="status" class="status"></div>
	</section>
	<section class="grid">
		<div class="panel">
			<h2>ไอเทมบัฟ</h2>
			<textarea id="buff" placeholder="ไอดีไอเทม เช่น 14533"></textarea>
			<div class="hint">รับเฉพาะไอเทมที่อยู่ใน db/custom/ai_item_buff.txt</div>
		</div>
		<div class="panel">
			<h2>ของที่เก็บไว้ในตัว</h2>
			<textarea id="keep" placeholder="ไอดีไอเทมที่ห้ามฝากคลัง"></textarea>
			<div class="hint">ของในรายการนี้จะไม่ถูกฝากเข้าคลังตอนน้ำหนักเต็ม</div>
		</div>
		<div class="panel">
			<h2>ซื้อของอัตโนมัติ</h2>
			<textarea id="buy" placeholder="item_id,เหลือน้อยกว่า,ซื้อให้ถึง&#10;501,20,100"></textarea>
			<div class="hint">ตัวอย่าง: 501,20,100 หมายถึง ถ้าเหลือน้อยกว่า 20 ซื้อให้ถึง 100</div>
		</div>
	</section>
</main>
<script>
const $ = id => document.getElementById(id);
const setStatus = (text, bad=false) => { $("status").textContent = text; $("status").className = bad ? "status err" : "status"; };
const form = () => { const f = new FormData(); f.append("AID",$("aid").value.trim()); f.append("GID",$("gid").value.trim()); f.append("AuthToken",$("token").value.trim()); return f; };
const ids = text => text.split(/[\n, ]+/).map(x=>parseInt(x,10)).filter(x=>x>0);
const buyRows = text => text.split(/\n+/).map(row=>row.trim()).filter(Boolean).map(row=>{ const p=row.split(/[,\s]+/).map(x=>parseInt(x,10)); return {item_id:p[0]||0,min_amount:p[1]||0,target_amount:p[2]||0}; }).filter(x=>x.item_id>0&&x.target_amount>x.min_amount);
$("load").onclick = async () => {
	try {
		const f = form();
		const r = await fetch("/autoattack/config/load", {method:"POST", body:f});
		if (!r.ok) throw new Error(await r.text());
		const d = await r.json();
		$("buff").value = (d.buff_items||[]).join("\n");
		$("keep").value = (d.keep_items||[]).join("\n");
		$("buy").value = (d.buy_items||[]).map(x=>`${x.item_id},${x.min_amount},${x.target_amount}`).join("\n");
		setStatus("โหลดค่าแล้ว");
	} catch(e) { setStatus(e.message || "โหลดไม่สำเร็จ", true); }
};
$("save").onclick = async () => {
	try {
		const f = form();
		f.append("data", JSON.stringify({buff_items: ids($("buff").value), keep_items: ids($("keep").value), buy_items: buyRows($("buy").value)}));
		const r = await fetch("/autoattack/config/save", {method:"POST", body:f});
		if (!r.ok) throw new Error(await r.text());
		setStatus("บันทึกแล้ว เข้าเกมใหม่หรือรอระบบโหลดค่าตัวละครอีกครั้ง");
	} catch(e) { setStatus(e.message || "บันทึกไม่สำเร็จ", true); }
};
</script>
</body>
</html>
)HTML";

	res.set_content(html, "text/html; charset=utf-8");
}

HANDLER_FUNC(autoattack_config_load)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	nlohmann::json response;
	response["buff_items"] = nlohmann::json::array();
	response["keep_items"] = nlohmann::json::array();
	response["buy_items"] = nlohmann::json::array();

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (SQL_ERROR == Sql_Query(handle,
		"SELECT `type`,`item_id`,`min_hp`,`min_sp` FROM `aa_items` WHERE `char_id` = %d AND `type` IN (0,3,4)",
		char_id)
	) {
		Sql_ShowDebug(handle);
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("โหลดข้อมูลไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	while (SQL_SUCCESS == Sql_NextRow(handle)) {
		char* data = nullptr;
		Sql_GetData(handle, 0, &data, nullptr);
		int type = atoi(data);
		Sql_GetData(handle, 1, &data, nullptr);
		int item_id = atoi(data);

		if (type == 0) {
			response["buff_items"].push_back(item_id);
		} else if (type == 3) {
			response["keep_items"].push_back(item_id);
		} else if (type == 4) {
			nlohmann::json row;
			row["item_id"] = item_id;
			Sql_GetData(handle, 2, &data, nullptr);
			row["min_amount"] = atoi(data);
			Sql_GetData(handle, 3, &data, nullptr);
			row["target_amount"] = atoi(data);
			response["buy_items"].push_back(row);
		}
	}

	Sql_FreeResult(handle);
	sl.unlock();
	res.set_content(response.dump(), "application/json");
}

HANDLER_FUNC(autoattack_config_save)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	if (!req.has_file("data")) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ไม่มีข้อมูล", "text/plain; charset=utf-8");
		return;
	}

	auto data = nlohmann::json::parse(req.get_file_value("data").content);
	auto buff_ids = autoattack_read_buff_item_ids();

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (SQL_ERROR == Sql_Query(handle, "START TRANSACTION")
		|| SQL_ERROR == Sql_Query(handle, "DELETE FROM `aa_items` WHERE `char_id` = %d AND `type` IN (0,3,4)", char_id)
	) {
		Sql_ShowDebug(handle);
		Sql_Query(handle, "ROLLBACK");
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("บันทึกไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	bool ok = true;
	for (const auto& item : data.value("buff_items", nlohmann::json::array())) {
		int item_id = item.get<int>();
		if (item_id <= 0 || buff_ids.find(item_id) == buff_ids.end())
			continue;

		if (SQL_ERROR == Sql_Query(handle,
			"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES (%d,0,%d)",
			char_id, item_id)
		) {
			Sql_ShowDebug(handle);
			ok = false;
			break;
		}
	}

	if (ok) {
		for (const auto& item : data.value("keep_items", nlohmann::json::array())) {
			int item_id = item.get<int>();
			if (item_id <= 0)
				continue;

			if (SQL_ERROR == Sql_Query(handle,
				"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`) VALUES (%d,3,%d)",
				char_id, item_id)
			) {
				Sql_ShowDebug(handle);
				ok = false;
				break;
			}
		}
	}

	if (ok) {
		for (const auto& item : data.value("buy_items", nlohmann::json::array())) {
			int item_id = item.value("item_id", 0);
			int min_amount = item.value("min_amount", 0);
			int target_amount = item.value("target_amount", 0);
			if (item_id <= 0 || min_amount < 0 || target_amount <= min_amount)
				continue;

			if (SQL_ERROR == Sql_Query(handle,
				"INSERT INTO `aa_items` (`char_id`,`type`,`item_id`,`min_hp`,`min_sp`) VALUES (%d,4,%d,%d,%d)",
				char_id, item_id, min_amount, target_amount)
			) {
				Sql_ShowDebug(handle);
				ok = false;
				break;
			}
		}
	}

	if (!ok || SQL_ERROR == Sql_Query(handle, "COMMIT")) {
		Sql_ShowDebug(handle);
		Sql_Query(handle, "ROLLBACK");
		sl.unlock();
		res.status = HTTP_BAD_REQUEST;
		res.set_content("บันทึกไม่ได้", "text/plain; charset=utf-8");
		return;
	}

	sl.unlock();
	res.set_content("{\"ok\":true}", "application/json");
}
