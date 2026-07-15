// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "autoattack_controller.hpp"

#include <cctype>
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

static int autoattack_find_latest_runtime_character(int account_id)
{
	int char_id = 0;
	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (SQL_SUCCESS == Sql_Query(handle,
		"SELECT `char_id` FROM `aa_runtime_status` WHERE `account_id` = %d ORDER BY `updated_at` DESC LIMIT 1",
		account_id)
	) {
		if (SQL_SUCCESS == Sql_NextRow(handle)) {
			char* data = nullptr;
			Sql_GetData(handle, 0, &data, nullptr);
			if (data)
				char_id = atoi(data);
		}
		Sql_FreeResult(handle);
	}

	sl.unlock();
	return char_id;
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
	bool has_aid = req.has_file("AID") || req.has_file("aid");
	bool has_gid = req.has_file("GID") || req.has_file("gid");
	if (!has_aid || !has_gid || !isAuthorized(req, false)) {
		res.status = HTTP_BAD_REQUEST;
		res.set_content("ไม่ได้รับอนุญาต", "text/plain; charset=utf-8");
		return false;
	}

	std::string aid_str = req.has_file("AID") ? req.get_file_value("AID").content : req.get_file_value("aid").content;
	std::string gid_str = req.has_file("GID") ? req.get_file_value("GID").content : req.get_file_value("gid").content;
	account_id = std::stoi(aid_str);
	char_id = std::stoi(gid_str);

	if (char_id <= 0)
		char_id = autoattack_find_latest_runtime_character(account_id);

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
		"`base_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`max_base_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`job_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`max_job_exp` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
		"`command` VARCHAR(32) NOT NULL DEFAULT '',"
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

static const char* autoattack_classic_page_html()
{
	return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Bot System</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#d7e3f2;font-family:Tahoma,Arial,sans-serif;font-size:12px;color:#17345a}.window{width:min(960px,100vw);min-height:620px;margin:0 auto;background:#edf5ff;border:1px solid #6b91bd;box-shadow:0 2px 10px #31506e}.title{height:27px;display:flex;align-items:center;justify-content:space-between;padding:0 8px;background:linear-gradient(#dfefff,#8bbbe8);border-bottom:1px solid #6897c4;font-weight:bold;color:#0b3364}.winbtn{display:inline-grid;place-items:center;width:18px;height:18px;margin-left:4px;border:1px solid #7b9bbd;background:#eef6ff;color:#2f5d8a}.tabs{display:flex;gap:3px;padding:6px 8px 0;background:#d9eafb;border-bottom:1px solid #9bb8d4;overflow:auto}.tab{min-width:74px;height:25px;border:1px solid #9bb8d4;border-bottom:0;background:linear-gradient(#fff,#dbe9f8);color:#284a70;cursor:pointer}.tab.active{background:#fff;color:#123e70;font-weight:bold}.body{display:grid;grid-template-columns:minmax(0,1fr) 280px;gap:12px;padding:10px;background:#f7fbff}.panel{border:1px solid #b5cce3;background:#fff;padding:10px;min-height:420px}.panel h3{margin:0 0 8px;font-size:13px;color:#004b91}.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:7px 0}.btn{min-width:112px;height:28px;border:1px solid #8baed1;background:linear-gradient(#fff,#d7e8fa);color:#16436c;cursor:pointer}.btn.primary{background:linear-gradient(#42c878,#168f44);border-color:#168f44;color:white;font-weight:bold}.btn.danger{background:linear-gradient(#ff8585,#d64040);border-color:#b82f2f;color:white}.btn.warn{background:linear-gradient(#ffd97d,#e7a92b);border-color:#bd8420;color:#352000}.btn:active{transform:translateY(1px)}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}.line{height:1px;background:#c7d9eb;margin:12px 0}.status{display:grid;grid-template-columns:90px 1fr;gap:5px 8px}.value{color:#0b7a32;font-weight:bold}.muted{color:#6c7890}.list{border:1px solid #d4e1ef;background:#f5f8fc;min-height:250px;padding:4px}.mob{display:grid;grid-template-columns:22px 1fr auto;align-items:center;height:27px;padding:0 6px}.mob:nth-child(even){background:#edf3fa}.off{color:#d31414}.on{color:#078d36}.footer{height:24px;display:flex;align-items:center;justify-content:space-between;padding:0 8px;border-top:1px solid #9bb8d4;background:#d9eafb}.notice{min-height:22px;color:#005dab}.input{height:26px;border:1px solid #9bb8d4;padding:0 7px;min-width:120px}.tabpage{display:none}.tabpage.active{display:block}@media(max-width:760px){.body{grid-template-columns:1fr}.window{min-height:100vh}.panel{min-height:0}.btn{min-width:98px}}
</style>
</head>
<body>
<div class="window">
	<div class="title"><span>AI Bot System Ver. 1.0</span><span><span class="winbtn">-</span><span class="winbtn">x</span></span></div>
	<div class="tabs">
		<button class="tab active" data-tab="control">Control</button><button class="tab" data-tab="combat">Combat</button><button class="tab" data-tab="teleport">Teleport</button><button class="tab" data-tab="skills">Skills</button><button class="tab" data-tab="storage">Storage</button><button class="tab" data-tab="support">Support</button><button class="tab" data-tab="loot">Loot</button><button class="tab" data-tab="buy">Buy NPC</button><button class="tab" data-tab="tools">Tools</button>
	</div>
	<div class="body">
		<div class="panel">
			<section id="control" class="tabpage active">
				<h3>Control</h3>
				<div class="grid"><button class="btn primary" data-cmd="toggle">Start / Stop AI</button><button class="btn danger" data-cmd="stop">Stop AI</button><button class="btn" data-cmd="storage">Go Storage</button><button class="btn" data-cmd="buy">Go Buy NPC</button></div>
				<div class="line"></div><h3>Quick Preset</h3>
				<div class="grid"><button class="btn" data-cmd="preset_farm">Farm Normal</button><button class="btn" data-cmd="preset_skill">Farm Skill</button><button class="btn warn" data-cmd="preset_safe">Safe Mode</button></div>
			</section>
			<section id="combat" class="tabpage">
				<h3>Map Monsters</h3>
				<div class="row"><button class="btn" data-cmd="mob_all_on">Attack all monsters</button><button class="btn" data-cmd="mob_all_off">Listed targets only</button><button class="btn danger" data-cmd="clear_mob">Clear Targets</button></div>
				<div class="list" id="monsterList"></div>
				<div class="line"></div><h3>Attack Mode</h3>
				<div class="grid"><button class="btn" data-cmd="attack_normal">Normal Attack</button><button class="btn" data-cmd="attack_skill">Skill Only</button><button class="btn" data-cmd="attack_support">Support / Idle</button></div>
			</section>
			<section id="teleport" class="tabpage">
				<h3>Teleport / Wing</h3>
				<div class="grid"><button class="btn" data-cmd="tele_on">Wing On</button><button class="btn" data-cmd="tele_off">Wing Off</button><button class="btn" data-cmd="preset_safe">Low HP Safe Preset</button></div>
			</section>
			<section id="skills" class="tabpage">
				<h3>Skills</h3>
				<div class="grid"><button class="btn" data-cmd="attack_skill">Use Attack Skills</button><button class="btn" data-cmd="attack_normal">Skill + Normal Attack</button><button class="btn" data-cmd="attack_support">Buff / Heal Only</button></div>
				<div class="line"></div><div class="muted">Deep skill ID setup is still handled by the in-game NPC menu.</div>
			</section>
			<section id="storage" class="tabpage">
				<h3>Storage</h3>
				<div class="grid"><button class="btn" data-cmd="storage">Store Now</button><button class="btn danger" data-cmd="clear_item">Clear Pickup Items</button></div>
			</section>
			<section id="support" class="tabpage">
				<h3>Support</h3>
				<div class="grid"><button class="btn" data-cmd="move_free">Move Free</button><button class="btn" data-cmd="move_stop">Stop Walk / Follow</button><button class="btn" data-cmd="party_on">Auto Party On</button><button class="btn" data-cmd="party_off">Auto Party Off</button><button class="btn" data-cmd="revive_on">Auto Revive On</button><button class="btn" data-cmd="revive_off">Auto Revive Off</button></div>
			</section>
			<section id="loot" class="tabpage">
				<h3>Loot</h3>
				<div class="grid"><button class="btn" data-cmd="pickup_all">Pickup All</button><button class="btn" data-cmd="pickup_list">Pickup List Only</button><button class="btn" data-cmd="pickup_none">Pickup None</button><button class="btn danger" data-cmd="clear_item">Clear Pickup List</button></div>
			</section>
			<section id="buy" class="tabpage">
				<h3>Buy NPC</h3>
				<div class="grid"><button class="btn" data-cmd="buy">Buy Now</button><button class="btn" data-cmd="storage">Store Before Buy</button></div>
			</section>
			<section id="tools" class="tabpage">
				<h3>Tools</h3>
				<div class="grid"><button class="btn danger" data-cmd="clear_all">Reset All AI Settings</button><button class="btn danger" data-cmd="clear_mob">Clear Monster Targets</button><button class="btn danger" data-cmd="clear_item">Clear Item Targets</button></div>
			</section>
		</div>
		<aside class="panel">
			<h3>Map Info</h3>
			<div class="status">
				<span>Character</span><span class="value" id="charName">-</span>
				<span>AI</span><span class="value" id="aiState">-</span>
				<span>Map</span><span id="mapName">-</span>
				<span>Position</span><span id="pos">-</span>
				<span>HP</span><span id="hp">-</span>
				<span>SP</span><span id="sp">-</span>
				<span>Weight</span><span id="weight">-</span>
				<span>Target</span><span id="target">-</span>
			</div>
			<div class="line"></div>
			<div class="row"><span>Target Mode</span><button class="btn primary" data-cmd="mob_all_on">ATK ALL</button></div>
			<div class="line"></div>
			<div class="notice" id="notice">Ready</div>
		</aside>
	</div>
	<div class="footer"><span>VIP Bot Time Remaining:</span><span>Developed by SafeZone</span></div>
</div>
<script>
const params=new URLSearchParams(location.search);
function form(cmd){const f=new FormData();f.append("AID",params.get("aid")||params.get("AID")||"");f.append("GID",params.get("gid")||params.get("GID")||"");f.append("AuthToken",params.get("token")||params.get("AuthToken")||"");if(cmd)f.append("cmd",cmd);return f}
function pct(a,b){return b>0?Math.round((a/b)*100):0}
function setNotice(s){document.getElementById("notice").textContent=s}
async function send(cmd){setNotice("Sending "+cmd+"...");try{const r=await fetch("/autoattack/status",{method:"POST",body:form(cmd)});if(!r.ok)throw new Error(await r.text());setNotice("Command sent: "+cmd);await load()}catch(e){setNotice("Error: "+e.message)}}
async function load(){try{const r=await fetch("/autoattack/status",{method:"POST",body:form()});if(!r.ok)throw new Error(await r.text());const d=await r.json();document.getElementById("charName").textContent=d.char_name||"-";document.getElementById("aiState").textContent=d.ai_active?"ON":"OFF";document.getElementById("mapName").textContent=d.map||"-";document.getElementById("pos").textContent=(d.x||0)+","+(d.y||0);document.getElementById("hp").textContent=(d.hp||0)+"/"+(d.max_hp||0)+" ("+pct(d.hp,d.max_hp)+"%)";document.getElementById("sp").textContent=(d.sp||0)+"/"+(d.max_sp||0)+" ("+pct(d.sp,d.max_sp)+"%)";document.getElementById("weight").textContent=(d.weight||0)+"/"+(d.max_weight||0);document.getElementById("target").textContent=d.target_name||d.target_id||"-";document.getElementById("monsterList").innerHTML=["Poring","Fabre","Pupa","Chonchon","Roda Frog","Lunatic","Green Plant"].map((n,i)=>`<div class=\"mob\"><span>${i+1}</span><span><b class=\"off\">[OFF]</b> ${n}</span><button class=\"btn\" data-cmd=\"mob_all_on\">ON</button></div>`).join("");document.querySelectorAll("#monsterList [data-cmd]").forEach(b=>b.onclick=()=>send(b.dataset.cmd))}catch(e){setNotice("Error: "+e.message)}}
document.querySelectorAll(".tab").forEach(b=>b.onclick=()=>{document.querySelectorAll(".tab,.tabpage").forEach(x=>x.classList.remove("active"));b.classList.add("active");document.getElementById(b.dataset.tab).classList.add("active")});
document.querySelectorAll("[data-cmd]").forEach(b=>b.onclick=()=>send(b.dataset.cmd));
load();setInterval(load,5000);
</script>
</body>
</html>)HTML";
}

HANDLER_FUNC(autoattack_page)
{
	res.set_content(autoattack_classic_page_html(), "text/html; charset=utf-8");
	return;

	static const char* html = R"HTML(<!doctype html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SafeZone AI Auto Farm</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700;800&family=Sarabun:wght@300;400;500;600;700&display=swap" rel="stylesheet">
<style>
:root {
  --bg-primary: #090d16;
  --bg-secondary: #111827;
  --bg-card: rgba(17, 24, 39, 0.75);
  --border-color: rgba(255, 255, 255, 0.08);
  --text-primary: #f3f4f6;
  --text-secondary: #9ca3af;
  --color-primary: #8b5cf6;
  --color-primary-glow: rgba(139, 92, 246, 0.25);
  --color-cyan: #06b6d4;
  --color-green: #10b981;
  --color-green-glow: rgba(16, 185, 129, 0.2);
  --color-orange: #f59e0b;
  --color-red: #ef4444;
  --transition-fast: 0.2s cubic-bezier(0.4, 0, 0.2, 1);
  --transition-normal: 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}

* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  background-color: var(--bg-primary);
  background-image: radial-gradient(circle at 50% 0%, rgba(139, 92, 246, 0.15) 0%, transparent 50%),
                    radial-gradient(circle at 100% 100%, rgba(6, 182, 212, 0.08) 0%, transparent 40%);
  background-attachment: fixed;
  color: var(--text-primary);
  font-family: 'Outfit', 'Sarabun', -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  min-height: 100vh;
  padding-bottom: 80px;
}

header {
  background: rgba(9, 13, 22, 0.85);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border-bottom: 1px solid var(--border-color);
  position: sticky;
  top: 0;
  z-index: 100;
  padding: 16px 24px;
}

.header-container {
  max-width: 1200px;
  margin: 0 auto;
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 16px;
}

.brand h1 {
  font-size: 22px;
  font-weight: 800;
  background: linear-gradient(135deg, #a855f7 0%, #06b6d4 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  display: inline-block;
  letter-spacing: 0.5px;
}

.brand p {
  font-size: 12px;
  color: var(--text-secondary);
  margin-top: 2px;
}

.nav-tabs {
  display: flex;
  gap: 6px;
  background: rgba(255, 255, 255, 0.03);
  padding: 4px;
  border-radius: 10px;
  border: 1px solid var(--border-color);
}

.tab-btn {
  background: transparent;
  border: 0;
  color: var(--text-secondary);
  font-family: inherit;
  font-size: 14px;
  font-weight: 600;
  padding: 8px 16px;
  border-radius: 8px;
  cursor: pointer;
  transition: var(--transition-fast);
}

.tab-btn:hover {
  color: var(--text-primary);
  background: rgba(255, 255, 255, 0.05);
}

.tab-btn.active {
  color: #fff;
  background: var(--color-primary);
  box-shadow: 0 4px 12px var(--color-primary-glow);
}

main {
  max-width: 1200px;
  margin: 24px auto 0;
  padding: 0 20px;
}

.tab-content {
  display: none;
}

.tab-content.active {
  display: block;
  animation: fadeIn 0.4s ease-out;
}

@keyframes fadeIn {
  from { opacity: 0; transform: translateY(8px); }
  to { opacity: 1; transform: translateY(0); }
}

.panel {
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--border-color);
  border-radius: 16px;
  padding: 24px;
  margin-bottom: 24px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.25);
}

.status-grid {
  display: grid;
  grid-template-columns: 1fr;
  gap: 16px;
  margin-bottom: 24px;
}

@media(min-width: 768px) {
  .status-grid {
    grid-template-columns: 1.2fr 1fr 1fr;
  }
}

.badge {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  border-radius: 999px;
  padding: 6px 14px;
  font-weight: 700;
  font-size: 13px;
  background: rgba(255, 255, 255, 0.05);
  color: var(--text-secondary);
  border: 1px solid transparent;
}

.badge.on {
  background: rgba(16, 185, 129, 0.12);
  color: var(--color-green);
  border: 1px solid rgba(16, 185, 129, 0.25);
  box-shadow: 0 0 12px rgba(16, 185, 129, 0.05);
  position: relative;
}

.badge.on::before {
  content: '';
  width: 8px;
  height: 8px;
  background: var(--color-green);
  border-radius: 50%;
  display: inline-block;
  animation: pulse-badge 1.5s infinite;
}

@keyframes pulse-badge {
  0% { transform: scale(0.9); opacity: 1; box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.7); }
  70% { transform: scale(1); opacity: 0.5; box-shadow: 0 0 0 6px rgba(16, 185, 129, 0); }
  100% { transform: scale(0.9); opacity: 1; box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
}

.badge.off {
  background: rgba(239, 68, 68, 0.1);
  color: var(--color-red);
  border: 1px solid rgba(239, 68, 68, 0.25);
}

.big-status {
  font-size: 28px;
  font-weight: 800;
  margin: 12px 0 6px;
  letter-spacing: -0.5px;
}

.dashboard-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}

@media(min-width: 768px) {
  .dashboard-grid {
    grid-template-columns: repeat(4, 1fr);
  }
}

.stat-box {
  background: rgba(255, 255, 255, 0.02);
  border: 1px solid var(--border-color);
  border-radius: 12px;
  padding: 16px;
  transition: var(--transition-fast);
}

.stat-box:hover {
  background: rgba(255, 255, 255, 0.04);
  border-color: rgba(255, 255, 255, 0.15);
  transform: translateY(-2px);
}

.stat-label {
  font-size: 11px;
  color: var(--text-secondary);
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.8px;
}

.stat-value {
  font-size: 17px;
  font-weight: 700;
  margin-top: 6px;
  color: var(--text-primary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.stat-bar-container {
  height: 6px;
  background: rgba(255, 255, 255, 0.05);
  border-radius: 99px;
  overflow: hidden;
  margin-top: 10px;
}

.stat-bar {
  display: block;
  height: 100%;
  border-radius: 99px;
  width: 0%;
  transition: width var(--transition-normal);
}

.stat-bar.hp {
  background: linear-gradient(90deg, #10b981, #059669);
  box-shadow: 0 0 8px rgba(16, 185, 129, 0.4);
}

.stat-bar.sp {
  background: linear-gradient(90deg, #3b82f6, #2563eb);
  box-shadow: 0 0 8px rgba(59, 130, 246, 0.4);
}

.stat-bar.exp {
  background: linear-gradient(90deg, #fbbf24, #f59e0b);
}

.stat-bar.jexp {
  background: linear-gradient(90deg, #a855f7, #7c3aed);
}

.stat-bar.weight {
  background: linear-gradient(90deg, #10b981, #f59e0b);
}

.stat-bar.weight.high {
  background: linear-gradient(90deg, #f59e0b, #ef4444);
  animation: pulse-red 1s infinite alternate;
}

@keyframes pulse-red {
  0% { box-shadow: 0 0 2px rgba(239, 68, 68, 0.4); }
  100% { box-shadow: 0 0 10px rgba(239, 68, 68, 0.8); }
}

.btn-container {
  display: flex;
  gap: 12px;
  margin-top: 20px;
  flex-wrap: wrap;
}

.btn-action {
  flex: 1;
  min-width: 200px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  font-family: inherit;
  font-size: 14px;
  font-weight: 700;
  padding: 14px 20px;
  border-radius: 12px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  transition: var(--transition-fast);
}

.btn-action:hover {
  background: rgba(255, 255, 255, 0.06);
  border-color: rgba(255, 255, 255, 0.15);
  transform: translateY(-1px);
}

.btn-action.storage-cmd {
  color: var(--color-orange);
  border-color: rgba(245, 158, 11, 0.3);
}

.btn-action.storage-cmd:hover {
  background: rgba(245, 158, 11, 0.1);
  border-color: var(--color-orange);
  box-shadow: 0 0 15px rgba(245, 158, 11, 0.15);
}

.btn-action.buy-cmd {
  color: #a855f7;
  border-color: rgba(168, 85, 247, 0.3);
}

.btn-action.buy-cmd:hover {
  background: rgba(168, 85, 247, 0.1);
  border-color: #a855f7;
  box-shadow: 0 0 15px rgba(168, 85, 247, 0.15);
}

/* Floating / Sticky Save Button */
.floating-save {
  position: fixed;
  bottom: 24px;
  right: 24px;
  background: linear-gradient(135deg, #10b981 0%, #059669 100%);
  border: 0;
  color: #fff;
  font-family: inherit;
  font-size: 15px;
  font-weight: 700;
  padding: 14px 28px;
  border-radius: 50px;
  cursor: pointer;
  box-shadow: 0 10px 25px rgba(16, 185, 129, 0.4);
  display: flex;
  align-items: center;
  gap: 8px;
  z-index: 99;
  transition: var(--transition-normal);
}

.floating-save:hover {
  transform: translateY(-4px) scale(1.03);
  box-shadow: 0 12px 30px rgba(16, 185, 129, 0.6);
  background: linear-gradient(135deg, #34d399 0%, #059669 100%);
}

.floating-save:active {
  transform: translateY(-1px) scale(0.98);
}

/* Items Management Styling */
.filter-bar {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-bottom: 20px;
}

@media(min-width: 768px) {
  .filter-bar {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
  }
}

.search-input-wrapper {
  position: relative;
  flex: 1;
  max-width: 400px;
}

.search-input-wrapper input {
  width: 100%;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  font-family: inherit;
  padding: 10px 16px;
  border-radius: 8px;
  outline: none;
  font-size: 14px;
  transition: var(--transition-fast);
}

.search-input-wrapper input:focus {
  background: rgba(255, 255, 255, 0.08);
  border-color: var(--color-primary);
  box-shadow: 0 0 10px rgba(139, 92, 246, 0.15);
}

.filter-buttons {
  display: flex;
  gap: 6px;
  overflow-x: auto;
  padding-bottom: 4px;
}

.filter-btn {
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--border-color);
  color: var(--text-secondary);
  font-family: inherit;
  font-size: 13px;
  font-weight: 600;
  padding: 6px 12px;
  border-radius: 6px;
  cursor: pointer;
  white-space: nowrap;
  transition: var(--transition-fast);
}

.filter-btn:hover {
  color: var(--text-primary);
  background: rgba(255, 255, 255, 0.06);
}

.filter-btn.active {
  background: rgba(139, 92, 246, 0.1);
  color: #a855f7;
  border-color: #a855f7;
}

.item-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
  gap: 16px;
}

.item-card {
  background: rgba(255, 255, 255, 0.02);
  border: 1px solid var(--border-color);
  border-radius: 14px;
  padding: 16px;
  text-align: center;
  transition: var(--transition-normal);
  display: flex;
  flex-direction: column;
  height: 100%;
}

.item-card:hover {
  transform: translateY(-4px);
  border-color: rgba(255, 255, 255, 0.15);
  background: rgba(255, 255, 255, 0.04);
  box-shadow: 0 10px 25px rgba(0, 0, 0, 0.15);
}

.item-card img {
  width: 52px;
  height: 52px;
  object-fit: contain;
  margin: 0 auto 12px;
  filter: drop-shadow(0 4px 6px rgba(0,0,0,0.3));
}

.item-id-badge {
  font-size: 11px;
  font-weight: 700;
  color: var(--text-secondary);
  background: rgba(255, 255, 255, 0.05);
  display: inline-block;
  padding: 3px 8px;
  border-radius: 6px;
  margin-bottom: 8px;
  font-family: monospace;
}

.item-label-input {
  width: 100%;
  background: transparent;
  border: 0;
  border-bottom: 1px dashed rgba(255, 255, 255, 0.15);
  color: var(--text-primary);
  font-family: inherit;
  font-weight: 600;
  font-size: 14px;
  text-align: center;
  margin-bottom: 6px;
  padding: 4px 0;
  outline: none;
  transition: var(--transition-fast);
}

.item-label-input:focus {
  border-bottom-style: solid;
  border-bottom-color: var(--color-primary);
}

.item-amt-info {
  font-size: 12px;
  font-weight: 600;
  color: var(--text-secondary);
  margin-bottom: 14px;
}

.item-opts {
  display: flex;
  flex-direction: column;
  gap: 8px;
  text-align: left;
  margin-top: auto;
}

.item-opts label {
  font-size: 13px;
  color: var(--text-secondary);
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  user-select: none;
  transition: var(--transition-fast);
}

.item-opts label:hover {
  color: var(--text-primary);
}

.item-opts input[type="checkbox"] {
  width: 16px;
  height: 16px;
  cursor: pointer;
  accent-color: var(--color-primary);
}

.buy-config-container {
  overflow: hidden;
  max-height: 0;
  transition: max-height var(--transition-normal);
  border-top: 1px dashed rgba(255, 255, 255, 0.05);
  margin-top: 8px;
  padding-top: 0;
}

.buy-config-container.show {
  max-height: 100px;
  padding-top: 10px;
  border-top-color: rgba(255, 255, 255, 0.08);
}

.buy-inputs {
  display: flex;
  gap: 8px;
}

.buy-inputs div {
  flex: 1;
}

.buy-inputs label {
  font-size: 10px;
  color: var(--text-secondary);
  display: block;
  margin-bottom: 4px;
  text-align: left;
}

.buy-inputs input {
  width: 100%;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--border-color);
  border-radius: 6px;
  color: var(--text-primary);
  font-family: inherit;
  font-size: 12px;
  padding: 6px;
  text-align: center;
  outline: none;
}

.buy-inputs input:focus {
  border-color: var(--color-primary);
  background: rgba(255, 255, 255, 0.06);
}

/* Item Header Panel */
.item-panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
  flex-wrap: wrap;
  gap: 12px;
}

.quick-add-box {
  display: flex;
  gap: 8px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--border-color);
  padding: 4px;
  border-radius: 10px;
}

.quick-add-box input {
  background: transparent;
  border: 0;
  color: var(--text-primary);
  padding: 8px 12px;
  outline: none;
  font-family: inherit;
  font-size: 14px;
  width: 160px;
}

.quick-add-box button {
  background: var(--color-primary);
  border: 0;
  color: #fff;
  font-family: inherit;
  font-weight: 700;
  font-size: 14px;
  padding: 8px 16px;
  border-radius: 8px;
  cursor: pointer;
  transition: var(--transition-fast);
}

.quick-add-box button:hover {
  background: #7c3aed;
}

/* Alert Notification */
.alert-msg {
  position: fixed;
  top: 24px;
  left: 50%;
  transform: translateX(-50%) translateY(-100px);
  z-index: 1000;
  background: rgba(16, 185, 129, 0.9);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  color: #fff;
  font-weight: 700;
  font-size: 14px;
  padding: 12px 24px;
  border-radius: 8px;
  box-shadow: 0 10px 25px rgba(0, 0, 0, 0.3);
  opacity: 0;
  transition: transform 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275), opacity 0.4s;
  display: flex;
  align-items: center;
  gap: 8px;
}

.alert-msg.show {
  transform: translateX(-50%) translateY(0);
  opacity: 1;
}

.alert-msg.error {
  background: rgba(239, 68, 68, 0.9);
}

/* Help Tab Card */
.help-card {
  background: rgba(255, 255, 255, 0.01);
  border: 1px solid var(--border-color);
  border-radius: 12px;
  padding: 20px;
  margin-bottom: 16px;
}

.help-card h3 {
  font-size: 16px;
  font-weight: 700;
  color: #fff;
  margin-bottom: 10px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.help-card p {
  color: var(--text-secondary);
  font-size: 14px;
  line-height: 1.6;
}

.help-card ul {
  padding-left: 20px;
  color: var(--text-secondary);
  font-size: 14px;
  line-height: 1.6;
  margin-top: 8px;
}

.help-card li {
  margin-bottom: 4px;
}
</style>
</head>
<body>
<!-- Alert Box -->
<div id="alertMsg" class="alert-msg"></div>

<header>
	<div class="header-container">
		<div class="brand">
			<h1>SafeZone AI Auto Farm</h1>
			<p>บอทฟาร์มและจัดการไอเทมแบบเรียลไทม์</p>
		</div>
		<div class="nav-tabs">
			<button class="tab-btn active" onclick="switchTab('dashboard')">📊 แผงควบคุม</button>
			<button class="tab-btn" onclick="switchTab('items')">📦 จัดการไอเทม</button>
			<button class="tab-btn" onclick="switchTab('help')">ℹ️ คู่มือการใช้งาน</button>
		</div>
	</div>
</header>

<main>
	<!-- Dashboard Tab Content -->
	<section id="tab-dashboard" class="tab-content active">
		<div class="panel">
			<div class="status-grid">
				<div>
					<span id="onlineBadge" class="badge off">ยังไม่มีข้อมูล</span>
					<div id="stateText" class="big-status">รอข้อมูล AI</div>
					<div class="stat-value" id="whereText" style="font-size:14px; font-weight:normal; color:var(--text-secondary);">เปิด AI ในเกมก่อน แล้วข้อมูลจะเริ่มแสดงผล</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">🎯 เป้าหมายปัจจุบัน</div>
					<div class="stat-value" id="targetText">-</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">🖐️ กำลังเก็บไอเทม</div>
					<div class="stat-value" id="pickupText">-</div>
				</div>
			</div>
			
			<div class="dashboard-grid">
				<div class="stat-box">
					<div class="stat-label">❤️ พลังชีวิต (HP)</div>
					<div class="stat-value" id="hpText">0 / 0</div>
					<div class="stat-bar-container">
						<span id="hpBar" class="stat-bar hp"></span>
					</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">💙 พลังจิต (SP)</div>
					<div class="stat-value" id="spText">0 / 0</div>
					<div class="stat-bar-container">
						<span id="spBar" class="stat-bar sp"></span>
					</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">🌟 Base EXP</div>
					<div class="stat-value" id="baseExpText" style="font-size:14px;">0 / 0</div>
					<div class="stat-bar-container">
						<span id="baseExpBar" class="stat-bar exp"></span>
					</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">✨ Job EXP</div>
					<div class="stat-value" id="jobExpText" style="font-size:14px;">0 / 0</div>
					<div class="stat-bar-container">
						<span id="jobExpBar" class="stat-bar jexp"></span>
					</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">⚖️ น้ำหนักกระเป๋า (Weight)</div>
					<div class="stat-value" id="weightText">0%</div>
					<div class="stat-bar-container" id="wBarWrap">
						<span id="weightBar" class="stat-bar weight"></span>
					</div>
				</div>
				<div class="stat-box">
					<div class="stat-label">💰 เงินในตัว (Zeny / CC)</div>
					<div class="stat-value" id="moneyText" style="font-size: 15px;">0 Z / 0 CC</div>
				</div>
				<div class="stat-box" style="grid-column: span 2;">
					<div class="stat-label">🔋 Stamina คงเหลือ</div>
					<div class="stat-value" id="staminaText" style="color:var(--color-orange); font-size:22px; font-weight:bold;">0 นาที 0 วินาที</div>
				</div>
			</div>
			
			<div class="btn-container">
				<button class="btn-action storage-cmd" onclick="sendCommand('storage')">📦 สั่งกลับเมืองฝากคลัง</button>
				<button class="btn-action buy-cmd" onclick="sendCommand('buy')">🛒 สั่งกลับเมืองซื้อของออโต้</button>
			</div>
			
			<div class="stat-label" id="lastText" style="margin-top:20px; text-align:right;">อัปเดตล่าสุด: -</div>
		</div>
	</section>

	<!-- Item Config Tab Content -->
	<section id="tab-items" class="tab-content">
		<div class="panel">
			<div class="item-panel-header">
				<div>
					<h2>📦 จัดการและตั้งค่าไอเทม</h2>
					<p class="stat-label" style="text-transform:none; font-weight:normal; margin-top:4px;">กำหนดไอเทมบัฟ ไอเทมเก็บในตัว และการซื้อยา/กระสุนออโต้</p>
				</div>
				<div class="quick-add-box">
					<input type="number" id="newItemId" placeholder="ใส่ ID ไอเทมเพื่อเพิ่ม...">
					<button type="button" onclick="addNewItem()">+ เพิ่ม</button>
				</div>
			</div>

			<div class="filter-bar">
				<div class="search-input-wrapper">
					<input type="text" id="searchInput" placeholder="🔍 ค้นหาไอเทมด้วย ID หรือชื่อเล่น..." oninput="filterItems()">
				</div>
				<div class="filter-buttons">
					<button class="filter-btn active" id="f-all" onclick="setFilter('all')">ทั้งหมด</button>
					<button class="filter-btn" id="f-buff" onclick="setFilter('buff')">ไอเทมบัฟ</button>
					<button class="filter-btn" id="f-keep" onclick="setFilter('keep')">ห้ามฝากคลัง</button>
					<button class="filter-btn" id="f-buy" onclick="setFilter('buy')">ซื้ออัตโนมัติ</button>
					<button class="filter-btn" id="f-inv" onclick="setFilter('inv')">มีในตัว</button>
				</div>
			</div>

			<div class="item-grid" id="itemGrid"></div>
		</div>
	</section>

	<!-- Help Tab Content -->
	<section id="tab-help" class="tab-content">
		<div class="panel">
			<h2 style="margin-bottom:20px;">📘 วิธีใช้งานระบบ AI ฟาร์ม</h2>
			
			<div class="help-card">
				<h3>🌟 การตั้งค่าไอเทมบัฟ (Auto Buff Items)</h3>
				<p>ติ๊กเลือก <b>"ไอเทมบัฟ"</b> บนไอเทมที่คุณต้องการให้ระบบใช้งานอัตโนมัติ (เช่น ยาเร่ง, ยาธาตุ, ใบชุบ, อาหารเพิ่มสเตตัส) เมื่อบัฟหมดเวลา บอทจะกดใช้ใหม่อัตโนมัติ</p>
				<p style="margin-top:8px; color:var(--color-orange); font-weight:bold;">⚠️ ข้อสำคัญ: ไอเทมบัฟที่ใช้งานได้จริง จะต้องอยู่ในรายการไอเทมบัฟที่เซิร์ฟเวอร์รองรับเท่านั้น</p>
			</div>

			<div class="help-card">
				<h3>🔒 ระบบห้ามฝากคลัง (Storage Exclusions)</h3>
				<p>ติ๊กเลือก <b>"ห้ามฝากคลัง"</b> บนอุปกรณ์หรือขยะชิ้นพิเศษที่คุณต้องการพกติดตัวตลอดเวลา เมื่อน้ำหนักเกินหรือสั่งการให้บอทกลับไปเก็บของ บอทจะ<b>ไม่</b>ดึงไอเทมเหล่านี้เข้าคลังฝากของ</p>
			</div>

			<div class="help-card">
				<h3>🛒 การสั่งซื้อไอเทมออโต้ (Auto Buy Supplies)</h3>
				<p>ติ๊กเลือก <b>"ซื้ออัตโนมัติ"</b> และกำหนด:</p>
				<ul>
					<li><b>เหลือน้อยกว่า:</b> เมื่อไอเทมลดลงต่ำกว่าจำนวนนี้ บอทจะบินกลับเมืองเพื่อเข้าซื้อทันที</li>
					<li><b>ซื้อให้ถึง:</b> จำนวนที่ต้องการซื้อพกติดตัวเมื่อไปร้านค้า</li>
				</ul>
				<p style="margin-top:8px;">ระบบนี้เหมาะสำหรับการตั้งค่า ลูกธนู (Arrows), ขวดยาปา (Acid/Grenade), ใบวิง, หินธาตุ, หรือยาปั๊มต่างๆ</p>
			</div>
			
			<div class="help-card">
				<h3>⌨️ สั่งการผ่านปุ่มควบคุม</h3>
				<p>คุณสามารถกดปุ่ม <b>"สั่งกลับเมืองฝากคลัง"</b> หรือ <b>"สั่งกลับเมืองซื้อของออโต้"</b> ได้ทันทีจากแดชบอร์ด โดยไม่ต้องรอให้น้ำหนักเต็มหรือของหมด บอทในเกมจะทำคำสั่งนั้นทันทีในวิงถัดไป</p>
			</div>
		</div>
	</section>
</main>

<!-- Unified Save Button -->
<button id="save" class="floating-save" style="display:none;">💾 บันทึกการตั้งค่า</button>

<script>
const $=id=>document.getElementById(id);

// Common RO Item Dictionary
const COMMON_ITEMS = {
	501: "Red Potion",
	502: "Orange Potion",
	503: "Yellow Potion",
	504: "White Potion",
	505: "Blue Potion",
	506: "Green Potion",
	507: "Red Herb",
	508: "Yellow Herb",
	509: "White Herb",
	510: "Blue Herb",
	511: "Green Herb",
	512: "Apple",
	513: "Banana",
	514: "Grape",
	515: "Carrot",
	516: "Potato",
	517: "Meat",
	518: "Raw Fish",
	519: "Pet Food",
	522: "Monster's Feed",
	523: "Candy",
	524: "Candy Cane",
	525: "Apple Juice",
	526: "Banana Juice",
	527: "Grape Juice",
	528: "Carrot Juice",
	529: "Honey",
	530: "Royal Jelly",
	601: "Fly Wing",
	602: "Butterfly Wing",
	603: "Yggdrasil Seed",
	607: "Yggdrasil Berry",
	616: "Old Blue Box",
	617: "Old Purple Box",
	644: "Gift Box",
	656: "Awakening Potion",
	657: "Berserk Potion",
	658: "Concentration Potion",
	713: "Oridecon",
	714: "Elunium",
	984: "Oridecon Hammer",
	985: "Elunium Hammer",
	1201: "Knife",
	1202: "Cutter",
	12028: "Poison Bottle",
	1750: "Arrow",
	1751: "Fire Arrow",
	1752: "Silver Arrow",
	1753: "Wind Arrow",
	1754: "Stone Arrow",
	1755: "Crystal Arrow",
	1756: "Shadow Arrow",
	1757: "Immaterial Arrow",
	1758: "Rusty Arrow",
	1759: "Poison Arrow",
	1760: "Sharp Arrow",
	1761: "Oridecon Arrow",
	1762: "Hunting Arrow",
	1763: "Arrow of Wind",
	1764: "Arrow of Fire",
	1765: "Arrow of Stone",
	1766: "Arrow of Water",
	1767: "Arrow of Holy",
	1768: "Arrow of Shadow",
	1769: "Arrow of Undead",
	1770: "Arrow of Poison",
	1771: "Arrow of Sleep",
	1772: "Arrow of Silence",
	1773: "Arrow of Curse",
	1774: "Arrow of Petrification",
	1775: "Arrow of Bleeding",
	1776: "Arrow of Stun",
	1777: "Arrow of Frozen",
	1778: "Arrow of Blindness"
};

// Switch navigation tabs
function switchTab(tabId) {
	document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
	document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));
	
	// Add active class
	event.currentTarget.classList.add('active');
	$('tab-' + tabId).classList.add('active');
	
	// Show save button only when on items tab
	if (tabId === 'items') {
		$('save').style.display = 'flex';
	} else {
		$('save').style.display = 'none';
	}
}

// Alert Message Helper
const msg = (text, isError = false) => {
	const alertBox = $('alertMsg');
	alertBox.textContent = text;
	if (isError) {
		alertBox.classList.add('error');
	} else {
		alertBox.classList.remove('error');
	}
	alertBox.classList.add('show');
	setTimeout(() => {
		alertBox.classList.remove('show');
	}, 4000);
};

// Parse parameters and forge request body
const p = new URLSearchParams(location.search);
const f = () => {
	const x = new FormData();
	x.append("AID", p.get("aid") || "");
	x.append("GID", p.get("gid") || "");
	x.append("AuthToken", p.get("token") || "");
	return x;
};

// Math helpers
const pct = (a, b) => b > 0 ? Math.max(0, Math.min(100, Math.round(a * 100 / b))) : 0;

// State translation
const stateName = s => ({
	attack: "⚔️ โจมตีมอนสเตอร์",
	pickup: "🖐️ กำลังเก็บของ",
	searching: "🔍 กำลังหาเป้าหมาย",
	resting: "💤 นั่งฟื้นฟู HP/SP",
	teleport: "🌀 วิงหาเป้าหมาย",
	danger: "🚨 วิ่งหนี / มอนรุม",
	dead: "💀 ตาย / รอคืนชีพ",
	storage: "📦 กลับเมืองฝากคลัง",
	buy: "🛒 กลับเมืองซื้อของ"
}[s] || s || "รอข้อมูล AI");

let allItems = new Set();
let buyData = {};
let itemAmounts = {};
let activeFilter = 'all';

// Load Config from APIs
async function loadConfig() {
	try {
		const r = await fetch("/autoattack/config/load", { method: "POST", body: f() });
		if (!r.ok) throw new Error(await r.text());
		const d = await r.json();

		allItems.clear();
		buyData = {};
		itemAmounts = {};
		$("itemGrid").innerHTML = "";

		(d.inventory || []).forEach(item => {
			allItems.add(item.id);
			itemAmounts[item.id] = item.amount;
		});

		(d.buff_items || []).forEach(id => allItems.add(id));
		(d.keep_items || []).forEach(id => allItems.add(id));
		(d.buy_items || []).forEach(b => {
			allItems.add(b.item_id);
			buyData[b.item_id] = { min: b.min_amount, target: b.target_amount };
		});

		Array.from(allItems).sort((a, b) => a - b).forEach(id => {
			renderItemCard(id,
				(d.buff_items || []).includes(id),
				(d.keep_items || []).includes(id),
				itemAmounts[id] || 0
			);
		});
	} catch (e) {
		msg(e.message || "โหลดการตั้งค่าไม่สำเร็จ", true);
	}
}

// Refresh Inventory dynamically
async function refreshInventory() {
	try {
		const r = await fetch("/autoattack/config/load", { method: "POST", body: f() });
		if (!r.ok) return;
		const d = await r.json();

		let newAmts = {};
		(d.inventory || []).forEach(item => newAmts[item.id] = item.amount);

		document.querySelectorAll(".item-card").forEach(card => {
			const id = parseInt(card.id.replace("card-", ""));
			const amt = newAmts[id] || 0;
			const amtDiv = card.querySelector(".item-amt-info");

			if (amtDiv) {
				amtDiv.textContent = amt > 0 ? `จำนวนในตัว: ${amt.toLocaleString()} ชิ้น` : `ไม่มีของในตัว`;
				amtDiv.style.color = amt > 0 ? "var(--color-green)" : "var(--text-secondary)";
			}
		});
	} catch (e) {}
}

// Render single item card
function renderItemCard(id, isBuff, isKeep, amount) {
	if ($("card-" + id)) return;
	const isBuy = !!buyData[id];
	const minA = isBuy ? buyData[id].min : '';
	const maxA = isBuy ? buyData[id].target : '';

	const amountColor = amount > 0 ? "var(--color-green)" : "var(--text-secondary)";
	const amountText = amount > 0 ? `จำนวนในตัว: ${amount.toLocaleString()} ชิ้น` : `ไม่มีของในตัว`;
	
	// Get local custom label
	const storedLabel = localStorage.getItem('aa_name_' + id) || COMMON_ITEMS[id] || '';

	const div = document.createElement("div");
	div.className = "item-card";
	div.id = "card-" + id;
	div.dataset.itemId = id;
	div.dataset.amount = amount;
	div.innerHTML = `
		<img src="https://static.divine-pride.net/images/items/item/${id}.png" onerror="this.src='https://static.divine-pride.net/images/items/item/512.png'">
		<div><span class="item-id-badge">ID: ${id}</span></div>
		<input type="text" class="item-label-input" placeholder="ตั้งชื่อไอเทม..." value="${storedLabel}" oninput="saveLocalName(${id}, this.value)">
		<div class="item-amt-info" style="color: ${amountColor}">${amountText}</div>
		<div class="item-opts">
			<label><input type="checkbox" class="cb-buff" value="${id}" ${isBuff ? 'checked' : ''}> 🧪 ไอเทมบัฟ</label>
			<label><input type="checkbox" class="cb-keep" value="${id}" ${isKeep ? 'checked' : ''}> 🔒 ห้ามฝากคลัง</label>
			<label><input type="checkbox" class="cb-buy" value="${id}" onchange="toggleBuy(${id})" ${isBuy ? 'checked' : ''}> 🛒 ซื้ออัตโนมัติ</label>
		</div>
		<div class="buy-config-container ${isBuy ? 'show' : ''}" id="buy-opt-${id}">
			<div class="buy-inputs">
				<div>
					<label>เหลือน้อยกว่า</label>
					<input type="number" class="in-min" placeholder="จำนวน" value="${minA}">
				</div>
				<div>
					<label>ซื้อให้ถึง</label>
					<input type="number" class="in-max" placeholder="จำนวน" value="${maxA}">
				</div>
			</div>
		</div>
	`;
	$("itemGrid").appendChild(div);
}

// Local storage for custom item names
function saveLocalName(id, val) {
	localStorage.setItem('aa_name_' + id, val);
}

// Show/Hide Buy Configuration
function toggleBuy(id) {
	const cb = document.querySelector(`#card-${id} .cb-buy`).checked;
	const container = document.getElementById(`buy-opt-${id}`);
	if (cb) {
		container.classList.add('show');
	} else {
		container.classList.remove('show');
	}
}

// Add new custom item
function addNewItem() {
	const id = parseInt($("newItemId").value);
	if (id > 0) {
		if (!allItems.has(id)) {
			allItems.add(id);
			renderItemCard(id, false, false, 0);
			filterItems(); // Re-apply current filter
			msg(`เพิ่มไอเทม ID ${id} แล้ว! กรอกชื่อและตั้งค่าที่การตั้งค่าด้านล่าง`);
		} else {
			msg(`ไอเทม ID ${id} มีอยู่แล้วในรายการ`, true);
		}
		$("newItemId").value = "";
	} else {
		msg("กรุณาใส่ ID ไอเทมที่ถูกต้อง", true);
	}
}

// Save Config to Server API
async function saveConfig() {
	try {
		const buff_items = [];
		const keep_items = [];
		const buy_items = [];

		document.querySelectorAll(".item-card").forEach(card => {
			const id = parseInt(card.id.replace("card-", ""));
			if (card.querySelector(".cb-buff").checked) buff_items.push(id);
			if (card.querySelector(".cb-keep").checked) keep_items.push(id);
			if (card.querySelector(".cb-buy").checked) {
				const minA = parseInt(card.querySelector(".in-min").value) || 0;
				const maxA = parseInt(card.querySelector(".in-max").value) || 0;
				if (maxA > minA) {
					buy_items.push({ item_id: id, min_amount: minA, target_amount: maxA });
				}
			}
		});

		const x = f();
		x.append("data", JSON.stringify({
			buff_items: buff_items,
			keep_items: keep_items,
			buy_items: buy_items
		}));

		const saveBtn = $("save");
		saveBtn.textContent = "⏳ กำลังบันทึก...";
		saveBtn.disabled = true;

		const r = await fetch("/autoattack/config/save", { method: "POST", body: x });
		if (!r.ok) throw new Error(await r.text());

		saveBtn.textContent = "💾 บันทึกการตั้งค่า";
		saveBtn.disabled = false;
		msg("บันทึกการตั้งค่าไอเทมเสร็จสมบูรณ์!");
	} catch (e) {
		const saveBtn = $("save");
		saveBtn.textContent = "💾 บันทึกการตั้งค่า";
		saveBtn.disabled = false;
		msg(e.message || "บันทึกไม่สำเร็จ", true);
	}
}

// Send control command
async function sendCommand(cmd) {
	try {
		const x = f();
		x.append("cmd", cmd);
		const r = await fetch("/autoattack/status", { method: "POST", body: x });
		if (r.ok) {
			const word = cmd === 'storage' ? "ส่งบอทกลับไปฝากคลังสำเร็จ บอทจะเริ่มทำในวิงถัดไป" : "ส่งบอทกลับไปซื้อของสำเร็จ บอทจะเริ่มทำในวิงถัดไป";
			msg(word);
		} else {
			msg("ส่งคำสั่งไม่สำเร็จ", true);
		}
	} catch (e) {
		msg("ส่งคำสั่งล้มเหลว", true);
	}
}

// Load real-time bot status
async function loadStatus() {
	try {
		const r = await fetch("/autoattack/status", { method: "POST", body: f() });
		if (!r.ok) throw new Error(await r.text());
		const d = await r.json();

		if (!d.has_data) {
			$("onlineBadge").textContent = "ยังไม่มีข้อมูล";
			$("onlineBadge").className = "badge off";
			return;
		}

		const online = !!d.online && !!d.ai_active;
		$("onlineBadge").textContent = online ? "🟢 AI กำลังทำงาน" : "🔴 หยุดทำงาน / ออฟไลน์";
		$("onlineBadge").className = online ? "badge on" : "badge off";
		$("stateText").textContent = online ? stateName(d.state) : "AI ปิดอยู่หรือออฟไลน์";
		$("whereText").textContent = `🌐 ${d.char_name || ""} @ ${d.map || "-"} (${d.x || 0}, ${d.y || 0})`;

		$("targetText").textContent = d.target_name ? `${d.target_name} (${d.target_id})` : "-";
		$("pickupText").textContent = d.pickup_item_name ? `${d.pickup_item_name} (${d.pickup_item_id})` : "-";

		$("hpText").textContent = `${(d.hp || 0).toLocaleString()} / ${(d.max_hp || 0).toLocaleString()}`;
		$("spText").textContent = `${(d.sp || 0).toLocaleString()} / ${(d.max_sp || 0).toLocaleString()}`;
		$("hpBar").style.width = pct(d.hp, d.max_hp) + "%";
		$("spBar").style.width = pct(d.sp, d.max_sp) + "%";

		$("baseExpText").textContent = `${(d.base_exp || 0).toLocaleString()} / ${(d.max_base_exp || 0).toLocaleString()}`;
		$("baseExpBar").style.width = pct(d.base_exp, d.max_base_exp) + "%";
		$("jobExpText").textContent = `${(d.job_exp || 0).toLocaleString()} / ${(d.max_job_exp || 0).toLocaleString()}`;
		$("jobExpBar").style.width = pct(d.job_exp, d.max_job_exp) + "%";

		const st_m = Math.floor((d.stamina || 0) / 60);
		const st_s = (d.stamina || 0) % 60;
		$("staminaText").textContent = `${st_m} นาที ${st_s} วินาที`;

		const wPct = pct(d.weight, d.max_weight);
		$("weightText").textContent = `${wPct}% (${(d.weight || 0).toLocaleString()} / ${(d.max_weight || 0).toLocaleString()})`;
		$("weightBar").style.width = wPct + "%";
		
		const weightBarSpan = $("weightBar");
		if (wPct >= 90) {
			weightBarSpan.className = "stat-bar weight high";
		} else {
			weightBarSpan.className = "stat-bar weight";
		}

		$("moneyText").textContent = `💰 ${(d.zeny || 0).toLocaleString()} Z | 💎 ${(d.cash || 0).toLocaleString()} CC`;
		$("lastText").textContent = `อัปเดตล่าสุด: ${d.age || 0} วินาทีที่แล้ว`;
	} catch (e) {}
}

// Client-side search and filters
function setFilter(filter) {
	activeFilter = filter;
	document.querySelectorAll('.filter-btn').forEach(btn => btn.classList.remove('active'));
	$('f-' + filter).classList.add('active');
	filterItems();
}

function filterItems() {
	const query = $('searchInput').value.toLowerCase().trim();
	
	document.querySelectorAll('.item-card').forEach(card => {
		const id = card.dataset.itemId;
		const amt = parseInt(card.dataset.amount) || 0;
		const localName = (localStorage.getItem('aa_name_' + id) || COMMON_ITEMS[id] || '').toLowerCase();
		
		const matchSearch = id.includes(query) || localName.includes(query);
		
		let matchFilter = true;
		if (activeFilter === 'buff') {
			matchFilter = card.querySelector('.cb-buff').checked;
		} else if (activeFilter === 'keep') {
			matchFilter = card.querySelector('.cb-keep').checked;
		} else if (activeFilter === 'buy') {
			matchFilter = card.querySelector('.cb-buy').checked;
		} else if (activeFilter === 'inv') {
			matchFilter = amt > 0;
		}
		
		if (matchSearch && matchFilter) {
			card.style.display = 'flex';
		} else {
			card.style.display = 'none';
		}
	});
}

// Event Bindings and Initialization
$("save").onclick = saveConfig;
if (p.has("aid") && p.has("gid") && p.has("token")) {
	loadConfig();
	loadStatus();
}

setInterval(loadStatus, 2500);
setInterval(refreshInventory, 3000);
</script>
</body>
</html>)HTML";

	res.set_content(html, "text/html; charset=utf-8");
}

HANDLER_FUNC(autoattack_scan_latest)
{
	int type = 0;
	if (req.has_param("type")) {
		try {
			type = std::stoi(req.get_param_value("type"));
		} catch (...) {
			type = 0;
		}
	}
	std::string path = "log/attackauto_scan_latest.txt";
	if (type >= 1 && type <= 11)
		path = "log/attackauto_scan_type_" + std::to_string(type) + ".txt";

	std::ifstream file(path, std::ios::binary);
	if (!file.good()) {
		res.set_content(type == 1 ? "0\tAll monsters\n" : "0\tNone\n", "text/plain; charset=utf-8");
		return;
	}

	std::ostringstream ss;
	ss << file.rdbuf();
	res.set_content(ss.str(), "text/plain; charset=utf-8");
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
	response["inventory"] = nlohmann::json::array();

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

	if (SQL_ERROR == Sql_Query(handle, "SELECT `nameid`, SUM(`amount`) FROM `inventory` WHERE `char_id` = %d AND `amount` > 0 GROUP BY `nameid`", char_id)) {
		Sql_ShowDebug(handle);
	} else {
		while (SQL_SUCCESS == Sql_NextRow(handle)) {
			char* data = nullptr;
			int item_id = 0, amount = 0;
			
			Sql_GetData(handle, 0, &data, nullptr);
			if (data) item_id = atoi(data);
			
			Sql_GetData(handle, 1, &data, nullptr);
			if (data) amount = atoi(data);

			if (item_id > 0) {
				nlohmann::json item_node;
				item_node["id"] = item_id;
				item_node["amount"] = amount;
				response["inventory"].push_back(item_node);
			}
		}
		Sql_FreeResult(handle);
	}

	sl.unlock();
	res.set_content(response.dump(), "application/json");
}

HANDLER_FUNC(autoattack_status)
{
	int account_id = 0, char_id = 0;
	if (!autoattack_require_auth(req, res, account_id, char_id))
		return;

	nlohmann::json response;
	response["has_data"] = false;

	SQLLock sl(MAP_SQL_LOCK);
	sl.lock();
	Sql* handle = sl.getHandle();

	if (req.has_file("cmd")) {
		std::string cmd = req.get_file_value("cmd").content;
		bool valid_cmd = !cmd.empty() && cmd.size() <= 31;
		for (char ch : cmd) {
			if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
				valid_cmd = false;
				break;
			}
		}
		if (valid_cmd)
			Sql_Query(handle, "UPDATE `aa_runtime_status` SET `command`='%s' WHERE `char_id`=%d", cmd.c_str(), char_id);
	}

	if (SQL_ERROR == Sql_Query(handle,
		"SELECT CAST(CONVERT(`char_name` USING utf8mb4) AS BINARY),`ai_active`,CAST(CONVERT(`state` USING utf8mb4) AS BINARY),CAST(CONVERT(`map` USING utf8mb4) AS BINARY),`x`,`y`,`hp`,`max_hp`,`sp`,`max_sp`,`weight`,`max_weight`,`zeny`,`cash`,`target_id`,CAST(CONVERT(`target_name` USING utf8mb4) AS BINARY),`pickup_item_id`,CAST(CONVERT(`pickup_item_name` USING utf8mb4) AS BINARY),TIMESTAMPDIFF(SECOND,`updated_at`,NOW()), `base_exp`, `max_base_exp`, `job_exp`, `max_job_exp` "
		"FROM `aa_runtime_status` WHERE `account_id` = %d AND `char_id` = %d LIMIT 1",
		account_id, char_id)
	) {
		Sql_FreeResult(handle);
		sl.unlock();
		res.status = 500;
		res.set_content("Database Error", "text/plain");
		return;
	}

	if (SQL_SUCCESS == Sql_NextRow(handle)) {
		char* data = nullptr;
		response["has_data"] = true;

		Sql_GetData(handle, 0, &data, nullptr); response["char_name"] = data ? data : "";
		Sql_GetData(handle, 1, &data, nullptr); response["ai_active"] = data && atoi(data) > 0;
		Sql_GetData(handle, 2, &data, nullptr); response["state"] = data ? data : "";
		Sql_GetData(handle, 3, &data, nullptr); response["map"] = data ? data : "";
		Sql_GetData(handle, 4, &data, nullptr); response["x"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 5, &data, nullptr); response["y"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 6, &data, nullptr); response["hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 7, &data, nullptr); response["max_hp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 8, &data, nullptr); response["sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 9, &data, nullptr); response["max_sp"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 10, &data, nullptr); response["weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 11, &data, nullptr); response["max_weight"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 12, &data, nullptr); response["zeny"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 13, &data, nullptr); response["cash"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 14, &data, nullptr); response["target_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 15, &data, nullptr); response["target_name"] = data ? data : "";
		Sql_GetData(handle, 16, &data, nullptr); response["pickup_item_id"] = data ? atoi(data) : 0;
		Sql_GetData(handle, 17, &data, nullptr); response["pickup_item_name"] = data ? data : "";
		Sql_GetData(handle, 18, &data, nullptr); int age = data ? atoi(data) : 9999; response["age"] = age;
		Sql_GetData(handle, 19, &data, nullptr); response["base_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 20, &data, nullptr); response["max_base_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 21, &data, nullptr); response["job_exp"] = data ? atoll(data) : 0;
		Sql_GetData(handle, 22, &data, nullptr); response["max_job_exp"] = data ? atoll(data) : 0;

		response["online"] = age <= 10;
	}
	Sql_FreeResult(handle);

	int stamina = 0, box_stamina = 0;
	if (SQL_SUCCESS == Sql_Query(handle, "SELECT `stamina_time`, `Boxstamina_time` FROM `stamina` WHERE `account_id` = %d", account_id)) {
		if (SQL_SUCCESS == Sql_NextRow(handle)) {
			char* sdata = nullptr;
			Sql_GetData(handle, 0, &sdata, nullptr); if(sdata) stamina = atoi(sdata);
			Sql_GetData(handle, 1, &sdata, nullptr); if(sdata) box_stamina = atoi(sdata);
		}
		Sql_FreeResult(handle);
	}
	response["stamina"] = stamina + box_stamina;

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
