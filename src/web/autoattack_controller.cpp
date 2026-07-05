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

static bool autoattack_ensure_status_table(Sql* handle)
{
	if (!handle)
		return false;

	if (SQL_ERROR == Sql_Query(handle,
		"CREATE TABLE IF NOT EXISTS `aa_runtime_status` ("
		"`char_id` INT(11) NOT NULL,"
		"`account_id` INT(11) NOT NULL,"
		"`char_name` VARCHAR(24) NOT NULL DEFAULT '',"
		"`ai_active` TINYINT(1) NOT NULL DEFAULT 0,"
		"`state` VARCHAR(64) NOT NULL DEFAULT '',"
		"`map` VARCHAR(32) NOT NULL DEFAULT '',"
		"`x` SMALLINT(6) NOT NULL DEFAULT 0,"
		"`y` SMALLINT(6) NOT NULL DEFAULT 0,"
		"`hp` INT(11) NOT NULL DEFAULT 0,"
		"`max_hp` INT(11) NOT NULL DEFAULT 0,"
		"`sp` INT(11) NOT NULL DEFAULT 0,"
		"`max_sp` INT(11) NOT NULL DEFAULT 0,"
		"`weight` INT(11) NOT NULL DEFAULT 0,"
		"`max_weight` INT(11) NOT NULL DEFAULT 0,"
		"`zeny` INT(11) NOT NULL DEFAULT 0,"
		"`cash` INT(11) NOT NULL DEFAULT 0,"
		"`target_id` INT(11) NOT NULL DEFAULT 0,"
		"`target_name` VARCHAR(64) NOT NULL DEFAULT '',"
		"`pickup_item_id` INT(11) NOT NULL DEFAULT 0,"
		"`pickup_item_name` VARCHAR(64) NOT NULL DEFAULT '',"
		"`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
		"PRIMARY KEY (`char_id`),"
		"KEY `account_id` (`account_id`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")
	) {
		Sql_ShowDebug(handle);
		return false;
	}

	return true;
}

HANDLER_FUNC(autoattack_page)
{
	static const char* mobile_html = R"HTML(
<!doctype html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>สถานะ AI</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:Tahoma,Arial,sans-serif;background:#f6f7f9;color:#1f2933}
header{background:#111827;color:#fff;padding:16px}main{max-width:980px;margin:0 auto;padding:14px}
h1{font-size:22px;margin:0}h2{font-size:17px;margin:0 0 10px}.muted{color:#6b7280;font-size:12px}
.panel{background:#fff;border:1px solid #d8dee8;border-radius:8px;padding:14px;margin-bottom:12px}
.statusTop{display:grid;grid-template-columns:1fr;gap:10px}@media(min-width:760px){.statusTop{grid-template-columns:1.3fr 1fr 1fr}}
.badge{display:inline-flex;align-items:center;gap:6px;border-radius:999px;padding:6px 10px;font-weight:bold;font-size:13px;background:#e5e7eb;color:#374151}
.badge.on{background:#dcfce7;color:#166534}.badge.off{background:#fee2e2;color:#991b1b}
.big{font-size:24px;font-weight:bold;margin:8px 0 4px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
@media(min-width:760px){.grid{grid-template-columns:repeat(4,1fr)}.settings{grid-template-columns:1fr 1fr 1fr}}
.box{border:1px solid #e5e7eb;border-radius:8px;padding:10px;background:#fafafa}.label{font-size:12px;color:#6b7280}.value{font-size:17px;font-weight:bold;margin-top:4px;word-break:break-word}
.bar{height:8px;background:#e5e7eb;border-radius:999px;overflow:hidden;margin-top:7px}.bar span{display:block;height:100%;background:#2563eb;width:0}
.settings{display:grid;grid-template-columns:1fr;gap:10px}label{display:block;font-size:13px;color:#4b5563;margin-bottom:5px}
input,textarea,button{font:inherit}input,textarea{width:100%;border:1px solid #cbd5e1;border-radius:7px;padding:10px;background:#fff}
textarea{min-height:130px;resize:vertical}button{border:0;border-radius:7px;padding:10px 13px;background:#2563eb;color:#fff;font-weight:bold}
button.secondary{background:#475569}.auth{display:grid;grid-template-columns:1fr;gap:8px}@media(min-width:760px){.auth{grid-template-columns:1fr 1fr 2fr auto auto}}
.msg{font-size:13px;margin-top:8px;color:#0f766e}.err{color:#b91c1c}
</style>
</head>
<body>
<header><h1>สถานะ AI</h1><div class="muted">ดูบอทจากมือถือแบบอัปเดตอัตโนมัติ</div></header>
<main>
	<section class="panel">
		<div class="auth" id="authbox">
			<div><label>AID</label><input id="aid" inputmode="numeric"></div>
			<div><label>Char ID</label><input id="gid" inputmode="numeric"></div>
			<div><label>Token</label><input id="token"></div>
			<button id="load">โหลดค่า</button>
			<button id="save" class="secondary">บันทึก</button>
		</div>
		<div id="message" class="msg"></div>
	</section>
	<section class="panel">
		<div class="statusTop">
			<div>
				<span id="onlineBadge" class="badge off">ยังไม่มีข้อมูล</span>
				<div id="stateText" class="big">รอข้อมูล AI</div>
				<div class="muted" id="whereText">เปิด AI ในเกมก่อน แล้วหน้านี้จะเริ่มอัปเดตเอง</div>
			</div>
			<div class="box"><div class="label">เป้าหมาย</div><div class="value" id="targetText">-</div></div>
			<div class="box"><div class="label">กำลังเก็บ</div><div class="value" id="pickupText">-</div></div>
		</div>
		<div class="grid" style="margin-top:10px">
			<div class="box"><div class="label">HP</div><div class="value" id="hpText">0 / 0</div><div class="bar"><span id="hpBar"></span></div></div>
			<div class="box"><div class="label">SP</div><div class="value" id="spText">0 / 0</div><div class="bar"><span id="spBar"></span></div></div>
			<div class="box"><div class="label">น้ำหนัก</div><div class="value" id="weightText">0%</div><div class="bar"><span id="weightBar"></span></div></div>
			<div class="box"><div class="label">เงิน</div><div class="value" id="moneyText">0 Z / 0 CC</div></div>
		</div>
		<div class="muted" id="lastText" style="margin-top:10px">อัปเดตล่าสุด: -</div>
	</section>
	<section class="settings">
		<div class="panel"><h2>ไอเทมบัฟ</h2><textarea id="buff" placeholder="ใส่ไอดีไอเทม บรรทัดละ 1 รายการ"></textarea><div class="muted">รับเฉพาะรายการใน ai_item_buff.txt</div></div>
		<div class="panel"><h2>ของที่ห้ามฝากคลัง</h2><textarea id="keep" placeholder="ไอดีไอเทมที่อยากเก็บไว้ในตัว"></textarea></div>
		<div class="panel"><h2>ซื้อของอัตโนมัติ</h2><textarea id="buy" placeholder="item_id,เหลือน้อยกว่า,ซื้อให้ถึง&#10;11573,20,100"></textarea><div class="muted">ตัวอย่าง: 11573,20,100</div></div>
	</section>
</main>
<script>
const $=id=>document.getElementById(id);
const msg=(t,bad=false)=>{$("message").textContent=t;$("message").className=bad?"msg err":"msg"};
const f=()=>{const x=new FormData();x.append("AID",$("aid").value.trim());x.append("GID",$("gid").value.trim());x.append("AuthToken",$("token").value.trim());return x};
const ids=t=>t.split(/[\n, ]+/).map(x=>parseInt(x,10)).filter(x=>x>0);
const buyRows=t=>t.split(/\n+/).map(r=>r.trim()).filter(Boolean).map(r=>{const p=r.split(/[,\s]+/).map(x=>parseInt(x,10));return{item_id:p[0]||0,min_amount:p[1]||0,target_amount:p[2]||0}}).filter(x=>x.item_id>0&&x.target_amount>x.min_amount);
const pct=(a,b)=>b>0?Math.max(0,Math.min(100,Math.round(a*100/b))):0;
const stateName=s=>({attack:"โจมตีมอนสเตอร์",pickup:"กำลังเก็บของ",searching:"หาเป้าหมาย",resting:"นั่งฟื้น HP/SP",teleport:"วิงหาเป้าหมาย",danger:"เจอมอนเข้ามาใกล้",dead:"ตาย / รอฟื้น",storage:"ฝากของเข้าคลัง",buy:"ซื้อของ / เปิดกล่อง"}[s]||s||"รอข้อมูล AI");
async function loadConfig(){try{const r=await fetch("/autoattack/config/load",{method:"POST",body:f()});if(!r.ok)throw new Error(await r.text());const d=await r.json();$("buff").value=(d.buff_items||[]).join("\n");$("keep").value=(d.keep_items||[]).join("\n");$("buy").value=(d.buy_items||[]).map(x=>`${x.item_id},${x.min_amount},${x.target_amount}`).join("\n");msg("โหลดค่าแล้ว")}catch(e){msg(e.message||"โหลดค่าไม่สำเร็จ",true)}}
async function saveConfig(){try{const x=f();x.append("data",JSON.stringify({buff_items:ids($("buff").value),keep_items:ids($("keep").value),buy_items:buyRows($("buy").value)}));const r=await fetch("/autoattack/config/save",{method:"POST",body:x});if(!r.ok)throw new Error(await r.text());msg("บันทึกแล้ว")}catch(e){msg(e.message||"บันทึกไม่สำเร็จ",true)}}
async function loadStatus(){try{const r=await fetch("/autoattack/status",{method:"POST",body:f()});if(!r.ok)throw new Error(await r.text());const d=await r.json();if(!d.has_data){$("onlineBadge").textContent="ยังไม่มีข้อมูล";$("onlineBadge").className="badge off";return}
const online=!!d.online&&!!d.ai_active;$("onlineBadge").textContent=online?"AI กำลังทำงาน":"ไม่พบการอัปเดต";$("onlineBadge").className=online?"badge on":"badge off";$("stateText").textContent=online?stateName(d.state):"AI เงียบหรือปิดอยู่";$("whereText").textContent=`${d.char_name||""} ${d.map||"-"} (${d.x||0}, ${d.y||0})`;
$("targetText").textContent=d.target_name?`${d.target_name} (${d.target_id})`:"-";$("pickupText").textContent=d.pickup_item_name?`${d.pickup_item_name} (${d.pickup_item_id})`:"-";
$("hpText").textContent=`${d.hp||0} / ${d.max_hp||0}`;$("spText").textContent=`${d.sp||0} / ${d.max_sp||0}`;$("weightText").textContent=`${pct(d.weight,d.max_weight)}%`;$("moneyText").textContent=`${(d.zeny||0).toLocaleString()} Z / ${(d.cash||0).toLocaleString()} CC`;
$("hpBar").style.width=pct(d.hp,d.max_hp)+"%";$("spBar").style.width=pct(d.sp,d.max_sp)+"%";$("weightBar").style.width=pct(d.weight,d.max_weight)+"%";$("lastText").textContent=`อัปเดตล่าสุด: ${d.age||0} วินาทีที่แล้ว`}catch(e){}}
$("load").onclick=loadConfig;$("save").onclick=saveConfig;
const p=new URLSearchParams(location.search);if(p.has("aid")&&p.has("gid")&&p.has("token")){$("aid").value=p.get("aid")||"";$("gid").value=p.get("gid")||"";$("token").value=p.get("token")||"";$("authbox").style.display="none";loadConfig();loadStatus();if(history.replaceState)history.replaceState(null,"","/autoattack")}
setInterval(loadStatus,2500);
</script>
</body>
</html>
)HTML";

	res.set_content(mobile_html, "text/html; charset=utf-8");
	return;

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
		<div class="auth" id="authbox">
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
const params = new URLSearchParams(location.search);
const hasDashboardAuth = params.has("aid") && params.has("gid") && params.has("token");
if (hasDashboardAuth) {
	$("aid").value = params.get("aid") || "";
	$("gid").value = params.get("gid") || "";
	$("token").value = params.get("token") || "";
	$("authbox").style.display = "none";
	setStatus("กำลังโหลดค่าจากเว็บสมาชิก...");
	$("load").click();
	if (history.replaceState)
		history.replaceState(null, "", "/autoattack");
}
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
	if (!autoattack_ensure_status_table(handle)) {
		sl.unlock();
		res.set_content(response.dump(), "application/json");
		return;
	}

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

HANDLER_FUNC(autoattack_status)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	nlohmann::json response;
	response["ok"] = true;
	response["has_data"] = false;
	response["online"] = false;
	response["ai_active"] = false;

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (SQL_ERROR == Sql_Query(handle,
		"SELECT `char_name`,`ai_active`,`state`,`map`,`x`,`y`,`hp`,`max_hp`,`sp`,`max_sp`,`weight`,`max_weight`,`zeny`,`cash`,`target_id`,`target_name`,`pickup_item_id`,`pickup_item_name`,TIMESTAMPDIFF(SECOND,`updated_at`,NOW()) "
		"FROM `aa_runtime_status` WHERE `account_id` = %d AND `char_id` = %d LIMIT 1",
		account_id, char_id)
	) {
		Sql_FreeResult(handle);
		sl.unlock();
		res.set_content(response.dump(), "application/json");
		return;
	}

	if (SQL_SUCCESS == Sql_NextRow(handle)) {
		char* data = nullptr;
		response["has_data"] = true;

		Sql_GetData(handle, 0, &data, nullptr);
		response["char_name"] = data ? data : "";
		Sql_GetData(handle, 1, &data, nullptr);
		response["ai_active"] = data && atoi(data) > 0;
		Sql_GetData(handle, 2, &data, nullptr);
		response["state"] = data ? data : "";
		Sql_GetData(handle, 3, &data, nullptr);
		response["map"] = data ? data : "";
		Sql_GetData(handle, 4, &data, nullptr);
		response["x"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 5, &data, nullptr);
		response["y"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 6, &data, nullptr);
		response["hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 7, &data, nullptr);
		response["max_hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 8, &data, nullptr);
		response["sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 9, &data, nullptr);
		response["max_sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 10, &data, nullptr);
		response["weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 11, &data, nullptr);
		response["max_weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 12, &data, nullptr);
		response["zeny"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 13, &data, nullptr);
		response["cash"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 14, &data, nullptr);
		response["target_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 15, &data, nullptr);
		response["target_name"] = data ? data : "";
		Sql_GetData(handle, 16, &data, nullptr);
		response["pickup_item_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 17, &data, nullptr);
		response["pickup_item_name"] = data ? data : "";
		Sql_GetData(handle, 18, &data, nullptr);
		int age = data ? atoi(data) : 9999;
		response["age"] = age;
		response["online"] = age <= 10;
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
