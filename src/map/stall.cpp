/***********************************/
/*********** Shakto      ********/
/** https://ronovelty.com/     **/
/***********************************/

#include "stall.hpp"

#include <cstdlib> // atoi
#include <sstream>

#include <common/malloc.hpp> // aMalloc, aFree
#include <common/nullpo.hpp>
#include <common/showmsg.hpp> // ShowInfo
#include <common/strlib.hpp>
#include <common/timer.hpp>  // DIFF_TICK

#include "achievement.hpp"
#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp" //mail send
#include "itemdb.hpp"
#include "log.hpp"
#include "npc.hpp"
#include "script.hpp" // [Addon] For add_str
#include "pc.hpp"
#include "pc_groups.hpp"
#include "vending.hpp"
#ifndef BUILDIN_FUNC
#define BUILDIN_FUNC(x) int buildin_ ## x (struct script_state* st)
#endif

//Stall
static int stall_id=START_STALL_NUM;
std::vector<s_stall_data *> stall_db;
std::vector<mail_message> stall_mail_db;
static const t_itemid STALL_CC_ITEM_ID = 30000;

static bool stall_trade_notice_table_ready = false;

static bool stall_ensure_trade_notice_table(void)
{
	if (stall_trade_notice_table_ready)
		return true;

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"CREATE TABLE IF NOT EXISTS `stall_trade_notice` ("
		"`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
		"`char_id` INT UNSIGNED NOT NULL,"
		"`account_id` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`direction` VARCHAR(16) NOT NULL,"
		"`channel` VARCHAR(16) NOT NULL,"
		"`currency` VARCHAR(8) NOT NULL,"
		"`amount` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`before_amount` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`after_amount` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`counterparty_id` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`counterparty_name` VARCHAR(24) NOT NULL DEFAULT '',"
		"`item_id` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`item_name` VARCHAR(64) NOT NULL DEFAULT '',"
		"`item_amount` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`unit_price` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`total_price` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`tax` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`claimed` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
		"`created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		"`claimed_at` DATETIME NULL,"
		"PRIMARY KEY (`id`),"
		"KEY `char_claim` (`char_id`,`claimed`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		Sql_ShowDebug(mmysql_handle);
		return false;
	}

	stall_trade_notice_table_ready = true;
	return true;
}

static const char* stall_currency_name(bool is_cash)
{
	return is_cash ? "CC" : "Zeny";
}

static const char* stall_item_display_name(t_itemid nameid)
{
	std::shared_ptr<item_data> id = item_db.find(nameid);
	return id ? id->name.c_str() : "Unknown item";
}

static int stall_count_inventory_item(map_session_data* sd, t_itemid nameid)
{
	int amount = 0;

	for (int i = 0; i < MAX_INVENTORY; i++) {
		if (sd->inventory.u.items_inventory[i].nameid == nameid)
			amount += sd->inventory.u.items_inventory[i].amount;
	}

	return amount;
}

static bool stall_del_inventory_item(map_session_data* sd, t_itemid nameid, int amount)
{
	if (amount <= 0)
		return true;
	if (stall_count_inventory_item(sd, nameid) < amount)
		return false;

	for (int i = 0; i < MAX_INVENTORY && amount > 0; i++) {
		if (sd->inventory.u.items_inventory[i].nameid != nameid)
			continue;

		int remove_amount = min(amount, sd->inventory.u.items_inventory[i].amount);
		if (pc_delitem(sd, i, remove_amount, 0, 0, LOG_TYPE_VENDING) != 0)
			return false;
		amount -= remove_amount;
	}

	return amount <= 0;
}

static bool stall_give_payout(map_session_data* sd, bool is_cash, int amount, int source_char_id, int& before_amount, int& after_amount)
{
	before_amount = 0;
	after_amount = 0;

	if (amount <= 0)
		return true;

	if (is_cash) {
		before_amount = stall_count_inventory_item(sd, STALL_CC_ITEM_ID);
		if (pc_checkadditem(sd, STALL_CC_ITEM_ID, amount) == CHKADDITEM_OVERAMOUNT)
			return false;

		struct item cc_item = {};
		cc_item.nameid = STALL_CC_ITEM_ID;
		cc_item.amount = amount;
		cc_item.identify = 1;
		if (pc_additem(sd, &cc_item, amount, LOG_TYPE_VENDING) != 0)
			return false;

		after_amount = before_amount + amount;
		return true;
	}

	before_amount = sd->status.zeny;
	if ((uint64)before_amount + (uint64)amount > (uint64)MAX_ZENY)
		return false;

	pc_getzeny(sd, amount, LOG_TYPE_VENDING, source_char_id);
	after_amount = sd->status.zeny;
	return true;
}

static void stall_notice_trade(map_session_data* sd, const char* action, const char* item_name, int item_amount, const char* counterparty_name, bool is_cash, int total_price, int payout, int before_amount, int after_amount)
{
	char msg[CHAT_SIZE_MAX];

	snprintf(msg, sizeof(msg), "Stall: %s %s x%d via %s, total %d, partner %s.",
		action, item_name, item_amount, stall_currency_name(is_cash), total_price, counterparty_name);
	clif_displaymessage(sd->fd, msg);

	if (payout > 0) {
		snprintf(msg, sizeof(msg), "Stall: received %d %s. Before %d, after %d.",
			payout, stall_currency_name(is_cash), before_amount, after_amount);
		clif_displaymessage(sd->fd, msg);
	}
}

static void stall_store_trade_notice(int char_id, int account_id, const char* direction, const char* channel, bool is_cash, int amount, int before_amount, int after_amount, int counterparty_id, const char* counterparty_name, t_itemid item_id, const char* item_name, int item_amount, int unit_price, int total_price, int tax, bool claimed)
{
	if (!stall_ensure_trade_notice_table())
		return;

	char esc_direction[48], esc_channel[48], esc_currency[24], esc_counterparty[NAME_LENGTH * 2], esc_item[128];
	Sql_EscapeString(mmysql_handle, esc_direction, direction);
	Sql_EscapeString(mmysql_handle, esc_channel, channel);
	Sql_EscapeString(mmysql_handle, esc_currency, stall_currency_name(is_cash));
	Sql_EscapeString(mmysql_handle, esc_counterparty, counterparty_name ? counterparty_name : "");
	Sql_EscapeString(mmysql_handle, esc_item, item_name ? item_name : "");

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"INSERT INTO `stall_trade_notice` "
		"(`char_id`,`account_id`,`direction`,`channel`,`currency`,`amount`,`before_amount`,`after_amount`,"
		"`counterparty_id`,`counterparty_name`,`item_id`,`item_name`,`item_amount`,`unit_price`,`total_price`,`tax`,`claimed`,`claimed_at`) "
		"VALUES (%d,%d,'%s','%s','%s',%d,%d,%d,%d,'%s',%u,'%s',%d,%d,%d,%d,%d,%s)",
		char_id, account_id, esc_direction, esc_channel, esc_currency, amount, before_amount, after_amount,
		counterparty_id, esc_counterparty, item_id, esc_item, item_amount, unit_price, total_price, tax,
		claimed ? 1 : 0, claimed ? "NOW()" : "NULL")) {
		Sql_ShowDebug(mmysql_handle);
	}
}

static void stall_queue_seller_payout(int seller_char_id, bool is_cash, int payout, int buyer_char_id, const char* buyer_name, t_itemid item_id, const char* item_name, int item_amount, int unit_price, int total_price, int tax)
{
	stall_store_trade_notice(seller_char_id, 0, "sale", "offline", is_cash, payout, 0, 0, buyer_char_id, buyer_name, item_id, item_name, item_amount, unit_price, total_price, tax, false);
}

void stall_process_pending_notice(map_session_data* sd)
{
	nullpo_retv(sd);

	if (!sd->state.pc_loaded || !stall_ensure_trade_notice_table())
		return;

	struct s_stall_pending_notice {
		uint64 id;
		char currency[8];
		int payout;
		int counterparty_id;
		char counterparty_name[NAME_LENGTH];
		int item_id;
		char item_name[64];
		int item_amount;
		int unit_price;
		int total_price;
		int tax;
	};
	std::vector<s_stall_pending_notice> pending;

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `id`,`currency`,`amount`,`counterparty_id`,`counterparty_name`,`item_id`,`item_name`,`item_amount`,`unit_price`,`total_price`,`tax` "
		"FROM `stall_trade_notice` WHERE `char_id`=%d AND `claimed`=0 ORDER BY `id` ASC LIMIT 20",
		sd->status.char_id)) {
		Sql_ShowDebug(mmysql_handle);
		return;
	}

	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		char *data = nullptr;
		unsigned long len = 0;
		s_stall_pending_notice row = {};

		Sql_GetData(mmysql_handle, 0, &data, &len); row.id = data ? strtoull(data, nullptr, 10) : 0;
		Sql_GetData(mmysql_handle, 1, &data, &len); safestrncpy(row.currency, data ? data : "", sizeof(row.currency));
		Sql_GetData(mmysql_handle, 2, &data, &len); row.payout = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 3, &data, &len); row.counterparty_id = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 4, &data, &len); safestrncpy(row.counterparty_name, data ? data : "", sizeof(row.counterparty_name));
		Sql_GetData(mmysql_handle, 5, &data, &len); row.item_id = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 6, &data, &len); safestrncpy(row.item_name, data ? data : "", sizeof(row.item_name));
		Sql_GetData(mmysql_handle, 7, &data, &len); row.item_amount = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 8, &data, &len); row.unit_price = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 9, &data, &len); row.total_price = data ? atoi(data) : 0;
		Sql_GetData(mmysql_handle, 10, &data, &len); row.tax = data ? atoi(data) : 0;
		pending.push_back(row);
	}

	Sql_FreeResult(mmysql_handle);

	for (const auto& row : pending) {
		bool is_cash = strcmp(row.currency, "CC") == 0;
		int before_amount = 0, after_amount = 0;
		if (!stall_give_payout(sd, is_cash, row.payout, row.counterparty_id, before_amount, after_amount)) {
			clif_displaymessage(sd->fd, "Stall: pending sale payout could not be received. Please free weight/space or reduce carried money.");
			continue;
		}

		stall_notice_trade(sd, "sold", row.item_name, row.item_amount, row.counterparty_name, is_cash, row.total_price, row.payout, before_amount, after_amount);
		if (row.tax > 0) {
			char msg[CHAT_SIZE_MAX];
			snprintf(msg, sizeof(msg), "Stall: tax %d %s was deducted from this sale.", row.tax, stall_currency_name(is_cash));
			clif_displaymessage(sd->fd, msg);
		}

		if (SQL_ERROR == Sql_Query(mmysql_handle,
			"UPDATE `stall_trade_notice` SET `claimed`=1,`before_amount`=%d,`after_amount`=%d,`account_id`=%d,`claimed_at`=NOW() WHERE `id`=%llu",
			before_amount, after_amount, sd->status.account_id, (unsigned long long)row.id)) {
			Sql_ShowDebug(mmysql_handle);
		}
	}
}

/// failure constants for clif functions
enum e_buyingstore_failure
{
	BUYINGSTORE_CREATE               = 1,  
	BUYINGSTORE_CREATE_OVERWEIGHT    = 2,  
	BUYINGSTORE_TRADE_BUYER_ZENY     = 3,  
	BUYINGSTORE_TRADE_BUYER_NO_ITEMS = 4,  
	BUYINGSTORE_TRADE_SELLER_FAILED  = 5,  
	BUYINGSTORE_TRADE_SELLER_COUNT   = 6,  
	BUYINGSTORE_TRADE_SELLER_ZENY    = 7,  
	BUYINGSTORE_CREATE_NO_INFO       = 8,  
};

static const t_itemid buyingstore_blankslots[MAX_SLOTS] = { 0 };

static int stall_getuid(void)
{
	if( stall_id >= START_STALL_NUM && !map_blid_exists(stall_id) )
		return stall_id++;
	else {
		int base_id = stall_id;
		while( base_id != ++stall_id ) {
			if( stall_id < START_STALL_NUM )
				stall_id = START_STALL_NUM;
			if( !map_blid_exists(stall_id) )
				return stall_id++;
		}
		ShowFatalError("stall_get_new_stall_id: All ids are taken. Exiting...");
		exit(1);
	}
}

int8 stall_ui_open(map_session_data* sd, uint16 skill_lv, short type){
	nullpo_retr(1, sd);

	if (sd->state.vending || sd->state.buyingstore || sd->state.trading) {
		return 1;
	}

	if( sd->sc.getSCE(SC_NOCHAT) && (sd->sc.getSCE(SC_NOCHAT)->val1&MANNER_NOROOM) )
	{
		return 2;
	}

	if( map_getmapflag(sd->bl.m, MF_NOVENDING) )
	{
		clif_displaymessage(sd->fd, msg_txt(sd,276)); 
		return 3;
	}

	extern int city_faction_war;
	if (city_faction_war == 1 && battle_config.faction_vending_block == 1) {
		int my_faction = pc_readaccountreg(sd, add_str("#city_faction"));
		struct map_data *mapdata = map_getmapdata(sd->bl.m);
		if (mapdata != nullptr) {
			if (strcmp(mapdata->name, "prontera") == 0 && my_faction != 2) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3004)); 
				return 3;
			}
			else if (strcmp(mapdata->name, "morocc") == 0 && my_faction != 1) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3005)); 
				return 3;
			}
		}
	}

	if(type == 0)
		clif_stall_vending_open(sd);
	else
		clif_stall_buying_open(sd);

	return 0;
}

int8 stall_vending_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, uint8 *data, int count)
{
	int i, j, k, l;
	char message_sql[MESSAGE_SIZE*2];
	StringBuf buf;
	struct block_list npc_near_bl;

	nullpo_retr(false,sd);

	if ( pc_isdead(sd) || !sd->state.prevend || pc_istrading(sd) ) { 
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		return 1; 
	}

	if(stall_isStallOpen(sd->status.char_id)){
		clif_displaymessage(sd->fd, "You can't open 2 stalls at the same time on a char.");
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		return 1;
	}

	extern int city_faction_war;
	if (city_faction_war == 1 && battle_config.faction_vending_block == 1) {
		int my_faction = pc_readaccountreg(sd, add_str("#city_faction"));
		struct map_data *mapdata = map_getmapdata(sd->bl.m);
		if (mapdata != nullptr) {
			if (strcmp(mapdata->name, "prontera") == 0 && my_faction != 2) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3004)); 
				clif_stall_ui_close(sd,100,STALLSTORE_OK);
				return 1;
			}
			else if (strcmp(mapdata->name, "morocc") == 0 && my_faction != 1) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3005)); 
				clif_stall_ui_close(sd,100,STALLSTORE_OK);
				return 1;
			}
		}
	}

	if( count < 1 || count > 2 + sd->stallvending_level ) { 
		clif_skill_fail(sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		return 3;
	}

	if( map_getcell(sd->bl.m,xPos,yPos,CELL_CHKNOVENDING) ) {
		clif_stall_ui_close(sd,100,2);
		return 1;
	}

	npc_near_bl.m = sd->bl.m;
	npc_near_bl.x = xPos;
	npc_near_bl.y = yPos;
	if( npc_isnear(&npc_near_bl) ) {
		char output[150];
		sprintf(output, msg_txt(sd,662), battle_config.min_npc_vendchat_distance);
		clif_displaymessage(sd->fd, output);
		clif_stall_ui_close(sd,100,2);
		return true;
	}

	int currency_mode = pc_readglobalreg(sd, add_str("stall_currency"));
	char final_message[MESSAGE_SIZE];
	
	if (currency_mode == 1) {
		snprintf(final_message, MESSAGE_SIZE, "[CASH] %s", message);
	} else {
		snprintf(final_message, MESSAGE_SIZE, "[ZENY] %s", message);
	}
	
	pc_setglobalreg(sd, add_str("stall_currency"), 0);

	struct s_stall_data *st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
	st->vended_id = sd->status.char_id; 

	if (save_settings&CHARSAVE_VENDING) 
		chrif_save(sd, CSAVE_INVENTORY);

	i = 0;
	for( j = 0; j < count; j++ ) {
		short index        = *(uint16*)(data + 8*j + 0);
		short amount       = *(uint16*)(data + 8*j + 2);
		unsigned int value = *(uint32*)(data + 8*j + 4);

		index = index - 2; 

		if( index < 0 || index >= MAX_INVENTORY 
		||  amount <= 0 
		||  sd->inventory.u.items_inventory[index].amount < amount 
		||  !sd->inventory.u.items_inventory[index].identify
		||  sd->inventory.u.items_inventory[index].attribute == 1 
		||  sd->inventory.u.items_inventory[index].expire_time 
		||  (sd->inventory.u.items_inventory[index].bound && !pc_can_give_bounded_items(sd)) 
		||  !itemdb_cantrade(&sd->inventory.u.items_inventory[index], pc_get_group_level(sd), pc_get_group_level(sd)) ) 
			continue;

		struct item temp_item;
		memcpy(&temp_item, &sd->inventory.u.items_inventory[index], sizeof(struct item));

		if( pc_delitem(sd, index, amount, 0, 0, LOG_TYPE_VENDING) != 0 )
			continue; 

		memcpy(&st->items_inventory[i], &temp_item, sizeof(struct item));
		st->items_inventory[i].amount = amount;
		st->price[i] = min(value, (unsigned int)battle_config.vending_max_value);
		i++;
	}

	if (i != j || j > MAX_STALL_SLOT) {
		clif_displaymessage(sd->fd, msg_txt(sd, 266)); 
		clif_skill_fail(sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		stall_vending_getbackitems(st); 
		aFree(st);
		return 5;
	}

	if( i == 0 ) { 
		clif_skill_fail(sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		aFree(st);
		return 5;
	}

	st->type = 0; 
	st->vender_id = stall_getuid();
	st->vend_num = i;
	st->expire_time = sd->stall_expire_time;
	
	safestrncpy(st->message, final_message, MESSAGE_SIZE);
	safestrncpy(st->name, sd->status.name, NAME_LENGTH);

	st->bl.id = st->vender_id;
	st->bl.type = BL_STALL;
	st->bl.m = map_mapindex2mapid(sd->mapindex);
	st->bl.x = xPos;
	st->bl.y = yPos;

	st->vd.class_ = sd->vd.class_;
	st->vd.weapon = sd->vd.weapon;
	st->vd.shield = sd->vd.shield;
	st->vd.head_top = sd->vd.head_top;
	st->vd.head_mid = sd->vd.head_mid;
	st->vd.head_bottom = sd->vd.head_bottom;
	st->vd.hair_style = sd->vd.hair_style;
	st->vd.hair_color = sd->vd.hair_color;
	st->vd.cloth_color = sd->vd.cloth_color;
	st->vd.body_style = sd->vd.body_style;
	st->vd.sex = sd->vd.sex;

	Sql_EscapeString( mmysql_handle, message_sql, st->message );

	if( Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
								  "`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`,"
								  "`clothes_color`, `name`, `expire_time`) "
		"VALUES( %d, %d, %d, %d, '%c', '%d', %d, %d, '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d, '%s', %u  );",
		stalls_table, st->vender_id, st->vended_id, st->type, st->vd.class_, st->vd.sex == SEX_FEMALE ? 'F' : 'M', st->bl.m, st->bl.x, st->bl.y,
		message_sql, st->vd.hair_style, st->vd.hair_color, st->vd.body_style, st->vd.weapon, st->vd.shield, st->vd.head_top, st->vd.head_mid, st->vd.head_bottom,
		st->vd.cloth_color, st->name, st->expire_time) != SQL_SUCCESS ) {
		Sql_ShowDebug(mmysql_handle);
		clif_displaymessage(sd->fd, "Something went wrong, maybe you used symbols in shop name.");
		clif_skill_fail(sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		stall_vending_getbackitems(st);
		aFree(st);
		return 5;
	}

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`stalls_id`,`index`,`nameid`,`amount`,`identify`,`refine`,`attribute`",stalls_vending_items_table);
	for( l = 0; l < MAX_SLOTS; ++l )
		StringBuf_Printf(&buf, ", `card%d`", l);
	for( l = 0; l < MAX_ITEM_RDM_OPT; ++l ) {
		StringBuf_Printf(&buf, ", `option_id%d`", l);
		StringBuf_Printf(&buf, ", `option_val%d`", l);
		StringBuf_Printf(&buf, ", `option_parm%d`", l);
	}
	StringBuf_Printf(&buf, ",`expire_time`,`bound`,`unique_id`,`enchantgrade`,`price`) VALUES", stalls_vending_items_table);
	for (j = 0; j < i; j++) {
		StringBuf_Printf(&buf, "(%d, %d, %u, %d, %d, %d, %d",
			st->vender_id, j, st->items_inventory[j].nameid, st->items_inventory[j].amount, st->items_inventory[j].identify, st->items_inventory[j].refine, st->items_inventory[j].attribute);
		for( k = 0; k < MAX_SLOTS; ++k )
			StringBuf_Printf(&buf, ", %u", st->items_inventory[j].card[k]);
		for( k = 0; k < MAX_ITEM_RDM_OPT; ++k ) {
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].id);
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].value);
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].param);
		}
		StringBuf_Printf(&buf, ", %u, %d , '%" PRIu64 "', %d, %d)",
			st->items_inventory[j].expire_time, st->items_inventory[j].bound, st->items_inventory[j].unique_id, st->items_inventory[j].enchantgrade, st->price[j]);
		if (j < i-1)
			StringBuf_AppendStr(&buf, ",");
	}
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf))){
		Sql_ShowDebug(mmysql_handle);
		clif_displaymessage(sd->fd, "Something went wrong on stall setting.");
		clif_skill_fail(sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		stall_vending_getbackitems(st);
		aFree(st);
		return 5;
	}
	StringBuf_Destroy(&buf);

	st->timer = add_timer(gettick() + (st->expire_time - time(NULL)) * 1000,
				stall_timeout, st->bl.id, 0);

	clif_stall_showunit(sd,st);
	clif_showstallboard(&sd->bl,st->vender_id,st->message);
	clif_stall_ui_close(sd,100,STALLSTORE_OK);

	if(map_addblock(&st->bl))
		return -1;
	status_change_init(&st->bl);
	map_addiddb(&st->bl);
	stall_db.push_back(st);

	return 0;
}

int8 stall_buying_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, const struct STALL_BUYING_SET_sub* itemlist, int count, uint64 total_price)
{
	int i, j, weight, listidx;
	char message_sql[MESSAGE_SIZE*2];
	StringBuf buf;
	struct block_list npc_near_bl;

	nullpo_retr(false,sd);

	if ( pc_isdead(sd) || !sd->state.prevend || pc_istrading(sd) ) { 
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 1; 
	}

	if(stall_isStallOpen(sd->status.char_id)){
		clif_displaymessage(sd->fd, "You can't open 2 stalls at the same time on a char.");
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 1;
	}

	extern int city_faction_war;
	if (city_faction_war == 1 && battle_config.faction_vending_block == 1) {
		int my_faction = pc_readaccountreg(sd, add_str("#city_faction"));
		struct map_data *mapdata = map_getmapdata(sd->bl.m);
		if (mapdata != nullptr) {
			if (strcmp(mapdata->name, "prontera") == 0 && my_faction != 2) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3004)); 
				clif_stall_ui_close(sd,101,STALLSTORE_OK);
				return 1;
			}
			else if (strcmp(mapdata->name, "morocc") == 0 && my_faction != 1) {
				clif_displaymessage(sd->fd, msg_txt(sd, 3005)); 
				clif_stall_ui_close(sd,101,STALLSTORE_OK);
				return 1;
			}
		}
	}

	npc_near_bl.m = sd->bl.m;
	npc_near_bl.x = xPos;
	npc_near_bl.y = yPos;
	if( npc_isnear(&npc_near_bl) ) {
		char output[150];
		sprintf(output, msg_txt(sd,662), battle_config.min_npc_vendchat_distance);
		clif_displaymessage(sd->fd, output);
		clif_stall_ui_close(sd,101,STALLSTORE_POSITION);
		return true;
	}

	if( count < 1 || count > 2 + sd->stallvending_level ) { 
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 3;
	}

	if( map_getcell(sd->bl.m,xPos,yPos,CELL_CHKNOVENDING) ) {
		clif_displaymessage (sd->fd, msg_txt(sd,204)); 
		clif_stall_ui_close(sd,101,STALLSTORE_POSITION);
		return 1;
	}

	if(total_price <= 0){
		clif_displaymessage(sd->fd, "Buying prices can't be 0.");
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 1;
	}

	int currency_mode = pc_readglobalreg(sd, add_str("stall_currency"));
	char final_message[MESSAGE_SIZE];
	
	if (currency_mode == 1) {
		snprintf(final_message, MESSAGE_SIZE, "[CASH] %s", message);
	} else {
		snprintf(final_message, MESSAGE_SIZE, "[ZENY] %s", message);
	}
	
	pc_setglobalreg(sd, add_str("stall_currency"), 0);

	struct s_stall_data *st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
	st->vended_id = sd->status.char_id; 

	if (save_settings&CHARSAVE_VENDING) 
		chrif_save(sd, CSAVE_INVENTORY);

	weight = sd->weight;

	i = 0;
	uint64 temp_price = 0;
	for( j = 0; j < count; j++ ) {
		const struct STALL_BUYING_SET_sub *item = &itemlist[i];

		struct item_data* id = itemdb_search( item->itemId );

		if( id == NULL || item->count == 0 
		||  item->price <= 0 || item->price > BUYINGSTALL_MAX_PRICE 
		||  !id->flag.buyingstore || !itemdb_cantrade_sub( id, pc_get_group_level( sd ), pc_get_group_level( sd ) ) ) 
			continue;

		int idx = pc_search_inventory( sd, item->itemId );

		if( idx < 0 ){
			break;
		}

		if( sd->inventory.u.items_inventory[idx].amount + item->count > BUYINGSTALL_MAX_AMOUNT ){
			break;
		}

		if( j ){
			ARR_FIND( 0, j, listidx, sd->buyingstore.items[listidx].nameid == item->itemId );
			if( listidx != j ){
				ShowWarning( "stall_buying_setup: Found duplicate item on buying list.\n" );
				break;
			}
		}

		weight+= id->weight*item->count;
		st->itemId[i] = item->itemId;
		st->amount[i] = item->count;
		st->price[i]  = item->price;

		uint64 price = (uint64)item->count * (uint64)item->price;
		if (temp_price + price > (uint64)MAX_ZENY) {
			clif_displaymessage(sd->fd, "Total price exceeds the maximum limit.");
			clif_stall_ui_close(sd,101,STALLSTORE_OK);
			aFree(st);
			return 5;
		}
		temp_price += price;

		i++; 
	}

	if (i != j || j > MAX_STALL_SLOT) {
		clif_displaymessage(sd->fd, msg_txt(sd, 266)); 
		clif_skill_fail(sd, ALL_ASSISTANT_BUYING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		aFree(st); 
		return 5;
	}

	if( (sd->max_weight*90)/100 < weight )
	{
		clif_stall_ui_close(sd,101,STALLSTORE_OVERWEIGHT);
		aFree(st); 
		return 7;
	}

	if( i == 0 ) { 
		clif_skill_fail(sd, ALL_ASSISTANT_BUYING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		aFree(st);
		return 5;
	}

	if (currency_mode == 1) {
		if ((uint64)stall_count_inventory_item(sd, STALL_CC_ITEM_ID) < temp_price) {
			clif_displaymessage(sd->fd, "You don't have enough CC item.");
			clif_stall_ui_close(sd,101,STALLSTORE_OK);
			aFree(st);
			return 5;
		}
		if (!stall_del_inventory_item(sd, STALL_CC_ITEM_ID, (int)temp_price)) {
			clif_displaymessage(sd->fd, "Failed to pay CC item.");
			clif_stall_ui_close(sd,101,STALLSTORE_OK);
			aFree(st);
			return 5;
		}
	} else {
		if ((uint64)sd->status.zeny < temp_price) {
			clif_displaymessage(sd->fd, "You don't have enough Zeny.");
			clif_stall_ui_close(sd,101,STALLSTORE_OK);
			aFree(st);
			return 5;
		}
		pc_payzeny(sd, (int)temp_price, LOG_TYPE_BUYING_STORE, sd->status.char_id);
	}
	
	st->type = 1;
	st->vender_id = stall_getuid();
	st->vend_num = i;
	st->expire_time = sd->stall_expire_time;
	safestrncpy(st->message, final_message, MESSAGE_SIZE);
	safestrncpy(st->name, sd->status.name, NAME_LENGTH);

	st->bl.id = st->vender_id;
	st->bl.type = BL_STALL;
	st->bl.m = map_mapindex2mapid(sd->mapindex);
	st->bl.x = xPos;
	st->bl.y = yPos;

	st->vd.class_ = sd->vd.class_;
	st->vd.weapon = sd->vd.weapon;
	st->vd.shield = sd->vd.shield;
	st->vd.head_top = sd->vd.head_top;
	st->vd.head_mid = sd->vd.head_mid;
	st->vd.head_bottom = sd->vd.head_bottom;
	st->vd.hair_style = sd->vd.hair_style;
	st->vd.hair_color = sd->vd.hair_color;
	st->vd.cloth_color = sd->vd.cloth_color;
	st->vd.body_style = sd->vd.body_style;
	st->vd.sex = sd->vd.sex;

	Sql_EscapeString( mmysql_handle, message_sql, st->message );

	if( Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
								  "`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`,"
								  "`clothes_color`, `name`, `expire_time`) "
		"VALUES( %d, %d, %d, %d, '%c', '%d', %d, %d, '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d, '%s', %u  );",
		stalls_table, st->vender_id, st->vended_id, st->type, st->vd.class_, st->vd.sex == SEX_FEMALE ? 'F' : 'M', st->bl.m, st->bl.x, st->bl.y,
		message_sql, st->vd.hair_style, st->vd.hair_color, st->vd.body_style, st->vd.weapon, st->vd.shield, st->vd.head_top, st->vd.head_mid, st->vd.head_bottom,
		st->vd.cloth_color, st->name, st->expire_time) != SQL_SUCCESS ) {
		Sql_ShowDebug(mmysql_handle);
	}

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`stalls_id`,`nameid`,`amount`,`price`) VALUES",stalls_buying_items_table);
	for (j = 0; j < i; j++) {
		StringBuf_Printf(&buf, "(%d, %u, %d, %d)",
			st->vender_id, st->itemId[j], st->amount[j], st->price[j]);
		if (j < i-1)
			StringBuf_AppendStr(&buf, ",");
	}
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
		Sql_ShowDebug(mmysql_handle);
	StringBuf_Destroy(&buf);

	st->timer = add_timer(gettick() + (st->expire_time - time(NULL)) * 1000,
				stall_timeout, st->bl.id, 0);

	clif_stall_showunit(sd,st);
	clif_buyingstall_entry(&sd->bl,st->vender_id,st->message);
	clif_stall_ui_close(sd,101,STALLSTORE_OK);

	if(map_addblock(&st->bl))
		return -1;
	status_change_init(&st->bl);
	map_addiddb(&st->bl);
	stall_db.push_back(st);

	return 0;
}

bool stall_isStallOpen(unsigned int CID){
	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return CID == itst->vended_id;
					});
	if(itStalls != 	stall_db.end()){
		return true;
	}
	return false;
}

void stall_vending_listreq(map_session_data* sd, int id)
{
	nullpo_retv(sd);
	struct s_stall_data* st;

	if( (st = map_id2st(id)) == NULL )
		return;

	if (!pc_can_give_items(sd)) { 
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		return;
	}

	clif_stall_vending_list( sd, st );
}

void stall_buying_listreq(map_session_data* sd, int id)
{
	nullpo_retv(sd);
	struct s_stall_data* st;

	if( !battle_config.feature_buying_store || pc_istrading(sd) )
		return;

	if( !pc_can_give_items(sd) )
	{
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		return;
	}

	if( (st = map_id2st(id)) == NULL )
		return;

	clif_stall_buying_list( sd, st );
}

void stall_vending_purchasereq(map_session_data* sd, int aid, int uid, const uint8* data, int count)
{
	int i;
	uint64 w; 
	double z;
	struct s_stall_data* st = map_id2st(uid);

	nullpo_retv(sd);
	if( st == NULL ) return;

	if (sd->vended_id != st->vender_id) return;
	sd->vended_id = 0;

	bool is_cash = (strncmp(st->message, "[CASH]", 6) == 0);

	if(!stall_isStallOpen(st->vended_id)){
		clif_displaymessage(sd->fd, "This stall is not opened anymore.");
		return;
	}

	if( st->vender_id != uid || st->vended_id != aid ) { 
		clif_buyvending(sd, 0, 0, 6);  
		return;
	}

	if( !searchstore_queryremote(sd, st->vended_id) && (sd->bl.m != st->bl.m || !check_distance_bl(&sd->bl, &st->bl, AREA_SIZE)) )
		return; 

	if( count < 1 || count > MAX_STALL_SLOT || count > st->vend_num )
		return; 

	// ==========================================
	// 🔴 ระบบใหม่: สำหรับ Bot ผี (ID 6 ล้านขึ้นไป) - ไม่ใช้จดหมาย, ใช้ Zeny2
	// ==========================================
	if (st->vended_id >= 6000000) {
		for (int i = 0; i < count; i++) {
			short check_idx = *(uint16*)(data + 4*i + 2);
			for (int j = 0; j < i; j++) {
				if (check_idx == *(uint16*)(data + 4*j + 2)) {
					ShowWarning("Stall Exploit: ผู้เล่น %s พยายามบัค Packet!\n", sd->status.name);
					clif_buyvending(sd, 0, 0, 6);
					return;
				}
			}
		}

		z = 0.; 
		w = 0;  
		
		for( i = 0; i < count; i++ ) {
			short amount = *(uint16*)(data + 4*i + 0);
			short idx    = *(uint16*)(data + 4*i + 2);
			idx -= 1;

			if( amount <= 0 ) return;
			if( idx < 0 || idx >= st->vend_num ) return;
			if( st->items_inventory[idx].amount <= 0 ) return;
			if( amount > st->items_inventory[idx].amount ){
				clif_buyvending(sd, idx, st->items_inventory[idx].amount, 4); 
				return;
			}

			z += ((double)st->price[idx] * (double)amount);
			w += (uint64)itemdb_weight(st->items_inventory[idx].nameid) * (uint64)amount;

			uint8 chk_flag = pc_checkadditem(sd, st->items_inventory[idx].nameid, amount);
			if (chk_flag == CHKADDITEM_OVERAMOUNT) {
				clif_buyvending(sd, idx, amount, 2); 
				return; 
			}
		}

		if( z > sd->status.zeny || z < 0. || z > (double)MAX_ZENY ) {
			clif_buyvending(sd, 0, 0, 1); 
			return;
		}

		if( w + (uint64)sd->weight > (uint64)sd->max_weight ) {
			clif_buyvending(sd, 0, 0, 2); return;
		}

		// หัก Zeny จากตัวละครโดยตรง
		pc_payzeny(sd, (int)z, LOG_TYPE_VENDING, st->vended_id);

		for( i = 0; i < count; i++ ) {
			short amount = *(uint16*)(data + 4*i + 0);
			short idx    = *(uint16*)(data + 4*i + 2);
			idx -= 1;

			st->items_inventory[idx].amount -= amount;

			unsigned char flag = 0;
			if ((flag = pc_additem(sd, &st->items_inventory[idx], amount, LOG_TYPE_VENDING))) {
				clif_additem(sd, 0, 0, flag);
				if (pc_candrop(sd, &st->items_inventory[idx]))
					map_addflooritem(&st->items_inventory[idx], amount, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 0, 0);
			}
		}

		char msg_output[256];
		sprintf(msg_output, "You purchased items for %d Zeny.", (int)z);
		clif_displaymessage(sd->fd, msg_output);

		bool remain_items = false;
		for( i = 0; i < st->vend_num; i++ ){
			if(st->items_inventory[i].amount > 0){
				remain_items = true;
				break;
			}
		}

		if( save_settings&CHARSAVE_VENDING ) {
			chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
			if(!remain_items) stall_remove(st);
			else stall_vending_save(st);
		}
		return; 
	}

	// ==========================================
	// 🔵 ระบบเก่า: ผู้เล่นทั่วไปตั้งร้าน Offline (ส่งจดหมายตามปกติ)
	// ==========================================
	for (int i = 0; i < count; i++) {
		short check_idx = *(uint16*)(data + 4*i + 2);
		for (int j = 0; j < i; j++) {
			if (check_idx == *(uint16*)(data + 4*j + 2)) {
				ShowWarning("Stall Exploit: player %s tried to duplicate purchase packet!\n", sd->status.name);
				clif_buyvending(sd, 0, 0, 6);
				return;
			}
		}
	}

	z = 0.;
	w = 0;
	for( i = 0; i < count; i++ ) {
		short amount = *(uint16*)(data + 4*i + 0);
		short idx    = *(uint16*)(data + 4*i + 2);
		idx -= 1;

		if( amount <= 0 ) return;
		if( idx < 0 || idx >= st->vend_num ) return;
		if( st->items_inventory[idx].amount <= 0 ) return;

		z += ((double)st->price[idx] * (double)amount);
		if (is_cash) {
			if( z > (double)stall_count_inventory_item(sd, STALL_CC_ITEM_ID) || z < 0. || z > (double)MAX_ZENY ) {
				clif_buyvending(sd, idx, amount, 1);
				return;
			}
		} else {
			if( z > sd->status.zeny || z < 0. || z > (double)MAX_ZENY ) {
				clif_buyvending(sd, idx, amount, 1);
				return;
			}
		}
		w += (uint64)itemdb_weight(st->items_inventory[idx].nameid) * (uint64)amount;
		if( w + (uint64)sd->weight > (uint64)sd->max_weight ) {
			clif_buyvending(sd, idx, amount, 2);
			return;
		}

		uint8 chk_flag = pc_checkadditem(sd, st->items_inventory[idx].nameid, amount);
		if (chk_flag == CHKADDITEM_OVERAMOUNT) {
			clif_buyvending(sd, idx, amount, 2);
			return;
		}

		if( amount > st->items_inventory[idx].amount ){
			clif_buyvending(sd, idx, st->items_inventory[idx].amount, 4);
			return;
		}
	}

	if (is_cash) {
		if (!stall_del_inventory_item(sd, STALL_CC_ITEM_ID, (int)z)) {
			clif_buyvending(sd, 0, 0, 1);
			return;
		}
	} else {
		pc_payzeny(sd, (int)z, LOG_TYPE_VENDING, st->vended_id);
	}

	uint64 total_price = 0;
	struct s_stall_sold_notice {
		t_itemid item_id;
		char item_name[64];
		int item_amount;
		int unit_price;
		int total_price;
	};
	s_stall_sold_notice sold_items[MAX_STALL_SLOT] = {};

	for( i = 0; i < count; i++ ) {
		short amount = *(uint16*)(data + 4*i + 0);
		short idx    = *(uint16*)(data + 4*i + 2);
		idx -= 1;

		uint32 price = st->price[idx];
		uint64 total = (uint64)price * (uint64)amount;
		t_itemid item_id = st->items_inventory[idx].nameid;
		struct item sold_item = {};
		memcpy(&sold_item, &st->items_inventory[idx], sizeof(struct item));
		sold_item.amount = amount;

		sold_items[i].item_id = item_id;
		safestrncpy(sold_items[i].item_name, stall_item_display_name(item_id), sizeof(sold_items[i].item_name));
		sold_items[i].item_amount = amount;
		sold_items[i].unit_price = price;
		sold_items[i].total_price = (int)min(total, (uint64)MAX_ZENY);

		st->items_inventory[idx].amount -= amount;

		unsigned char flag = pc_additem(sd, &sold_item, amount, LOG_TYPE_VENDING);
		if (flag) {
			clif_additem(sd, 0, 0, flag);
			if (pc_candrop(sd, &sold_item))
				map_addflooritem(&sold_item, amount, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 0, 0);
		}

		total_price += total;
	}

	int total_tax_rate = battle_config.buy_vat;
	int is_town = 0;

	if (Sql_Query(mmysql_handle, "SELECT `tax_rate` FROM `mayor_towns` WHERE `town_map` = '%s' AND `is_active` = 1", map_mapid2mapname(st->bl.m)) == SQL_SUCCESS) {
		if (Sql_NumRows(mmysql_handle) > 0 && Sql_NextRow(mmysql_handle) == SQL_SUCCESS) {
			char* db_data; Sql_GetData(mmysql_handle, 0, &db_data, NULL);
			if (db_data) {
				int town_tax = atoi(db_data);
				if (town_tax > total_tax_rate) total_tax_rate = town_tax;
			}
			is_town = 1;
		}
		Sql_FreeResult(mmysql_handle);
	}

	int tax = 0;
	int treasury_income = 0;
	if (is_cash) {
		tax = (int)((total_price * (uint64)total_tax_rate) / 100);
		if (tax > 0)
			treasury_income = tax * battle_config.stall_cash_to_zeny_tax_rate;
	} else {
		if (total_price >= 100) {
			tax = (int)((total_price * (uint64)total_tax_rate) / 100);
			treasury_income = tax;
		}
	}

	if (tax > 0 && total_tax_rate > 0 && treasury_income > 0) {
		if (Sql_Query(mmysql_handle, "UPDATE `castle_money` SET `money` = `money` + %d WHERE `castle` = 0", treasury_income) != SQL_SUCCESS)
			Sql_ShowDebug(mmysql_handle);
		if (is_town) {
			if (Sql_Query(mmysql_handle, "UPDATE `mayor_towns` SET `treasury` = `treasury` + %d WHERE `town_map` = '%s'", treasury_income, map_mapid2mapname(st->bl.m)) != SQL_SUCCESS)
				Sql_ShowDebug(mmysql_handle);
		}
	}

	map_session_data* vendor_sd = map_charid2sd(st->vended_id);
	for (i = 0; i < count; i++) {
		int item_tax = total_price > 0 ? (int)(((uint64)tax * (uint64)sold_items[i].total_price) / total_price) : 0;
		int item_payout = sold_items[i].total_price - item_tax;
		int before_amount = 0, after_amount = 0;
		bool delivered = false;

		if (vendor_sd != nullptr)
			delivered = stall_give_payout(vendor_sd, is_cash, item_payout, sd->status.char_id, before_amount, after_amount);

		if (delivered) {
			stall_notice_trade(vendor_sd, "sold", sold_items[i].item_name, sold_items[i].item_amount, sd->status.name, is_cash, sold_items[i].total_price, item_payout, before_amount, after_amount);
			stall_store_trade_notice(vendor_sd->status.char_id, vendor_sd->status.account_id, "sale", "online", is_cash, item_payout, before_amount, after_amount, sd->status.char_id, sd->status.name, sold_items[i].item_id, sold_items[i].item_name, sold_items[i].item_amount, sold_items[i].unit_price, sold_items[i].total_price, item_tax, true);
		} else {
			stall_queue_seller_payout(st->vended_id, is_cash, item_payout, sd->status.char_id, sd->status.name, sold_items[i].item_id, sold_items[i].item_name, sold_items[i].item_amount, sold_items[i].unit_price, sold_items[i].total_price, item_tax);
		}

		stall_notice_trade(sd, "bought", sold_items[i].item_name, sold_items[i].item_amount, st->name, is_cash, sold_items[i].total_price, 0, 0, 0);
	}

	if (vendor_sd != nullptr && save_settings&CHARSAVE_VENDING)
		chrif_save(vendor_sd, CSAVE_INVENTORY);

	bool remain_items = false;
	for( i = 0; i < st->vend_num; i++ ){
		if(st->items_inventory[i].amount > 0){
			remain_items = true;
			break;
		}
	}

	if( save_settings&CHARSAVE_VENDING ) {
		chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
		if(!remain_items){
			stall_remove(st);
		} else
			stall_vending_save(st);
	}
}

void stall_buying_purchasereq(map_session_data* sd, int aid, int uid, const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* itemlist, unsigned int count )
{
	struct s_stall_data* st = map_id2st(uid);

	nullpo_retv(sd);
	if( st == NULL ) return; 

	if (sd->vended_id != st->vender_id) return;
	sd->vended_id = 0;

	bool is_cash = (strncmp(st->message, "[CASH]", 6) == 0);

	if(!stall_isStallOpen(st->vended_id)){
		clif_displaymessage(sd->fd, "This stall is not opened anymore.");
		return;
	}

	if( st->vender_id != uid || st->vended_id != aid ) { 
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !battle_config.feature_buying_store || pc_istrading(sd) )
	{
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !pc_can_give_items(sd) )
	{
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !searchstore_queryremote(sd, st->vended_id) && (sd->bl.m != st->bl.m || !check_distance_bl(&sd->bl, &st->bl, AREA_SIZE)) ){
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return; 
	}

	if( count < 1 || count > MAX_STALL_SLOT || count > st->vend_num )
		return; 

	// ==========================================
	// 🔴 ระบบใหม่: สำหรับ Bot ผี (ID 6 ล้านขึ้นไป) - ไม่ใช้จดหมาย, ใช้ Zeny2
	// ==========================================
	if (st->vended_id >= 6000000) {
		uint64 total_price = 0;
		for( int i = 0; i < count; i++ ){
			const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];

			for( int k = 0; k < i; k++ ){
				if( itemlist[k].index == item->index && k != i ){
					clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
					return;
				}
			}

			int index = item->index - 2; 
			if( item->amount <= 0 ) return;

			if( index < 0 || index >= ARRAYLENGTH( sd->inventory.u.items_inventory ) || sd->inventory_data[index] == NULL || sd->inventory.u.items_inventory[index].nameid != item->itemId || sd->inventory.u.items_inventory[index].amount < item->amount ){
				clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
				return;
			}

			if( sd->inventory.u.items_inventory[index].expire_time || ( sd->inventory.u.items_inventory[index].bound && !pc_can_give_bounded_items( sd ) ) || memcmp( sd->inventory.u.items_inventory[index].card, buyingstore_blankslots, sizeof( buyingstore_blankslots ) ) ){
				clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
				return;
			}

			int listidx = -1;
			for(int k = 0 ; k < st->vend_num; k++){
				if(item->itemId == st->itemId[k]){
					listidx = k;
					break;
				}
			}

			if( listidx == -1 || st->amount[listidx] <= 0 || st->amount[listidx] < item->amount){
				clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_COUNT, item->itemId );
				return;
			}

			total_price += (uint64)item->amount * (uint64)st->price[listidx];
		}

		int total_tax_rate = battle_config.buy_vat; 
		int tax = 0;
		if (total_price >= 100) {
			tax = (int)((total_price * (uint64)total_tax_rate) / 100);
		}
		uint64 final_earn = total_price - tax;

		if (((uint64)sd->status.zeny + final_earn > (uint64)MAX_ZENY)) { 
			clif_displaymessage(sd->fd, "You cannot hold more Zeny.");
			clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
			return;
		}

		for( int i = 0; i < count; i++ ){
			const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];
			int listidx = -1;
			for(int k = 0 ; k < st->vend_num; k++){
				if(item->itemId == st->itemId[k]){ listidx = k; break; }
			}
			int index = item->index - 2; 

			pc_delitem(sd, index, item->amount, 0, 0, LOG_TYPE_BUYING_STORE);
			st->amount[listidx] -= item->amount;
		}

		if (final_earn > 0) {
			pc_getzeny(sd, (int)final_earn, LOG_TYPE_BUYING_STORE, st->vended_id);
			char msg_output[256];
			sprintf(msg_output, "You sold items and received %d Zeny. (Tax: %d%%)", (int)final_earn, total_tax_rate);
			clif_displaymessage(sd->fd, msg_output);
		}

		bool remain_items = false;
		for( int i = 0; i < st->vend_num; i++ ){
			if(st->amount[i] > 0){
				remain_items = true;
				break;
			}
		}

		if( save_settings&CHARSAVE_VENDING ) {
			chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
			if(!remain_items) stall_remove(st);
			else stall_buying_save(st);
		}
		return; 
	}

	// ==========================================
	// 🔵 ระบบเก่า: ผู้เล่นทั่วไปรับซื้อ Offline (ส่งจดหมายตามปกติ)
	// ==========================================
	struct mail_message temp_mail_check;
	if (count > ARRAYLENGTH(temp_mail_check.item)) {
		clif_displaymessage(sd->fd, "You cannot sell this many different items at once due to mail system limits.");
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	for( int i = 0; i < count; i++ ){
		const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];

		for( int k = 0; k < i; k++ ){
			if( itemlist[k].index == item->index && k != i ){
				clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
				return;
			}
		}

		int index = item->index - 2; 

		if( item->amount <= 0 ) return;

		if( index < 0 || index >= ARRAYLENGTH( sd->inventory.u.items_inventory ) || sd->inventory_data[index] == NULL || sd->inventory.u.items_inventory[index].nameid != item->itemId || sd->inventory.u.items_inventory[index].amount < item->amount ){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
			return;
		}

		if( sd->inventory.u.items_inventory[index].expire_time || ( sd->inventory.u.items_inventory[index].bound && !pc_can_give_bounded_items( sd ) ) || memcmp( sd->inventory.u.items_inventory[index].card, buyingstore_blankslots, sizeof( buyingstore_blankslots ) ) ){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
			return;
		}

		int listidx = -1;
		for(int k = 0 ; k < st->vend_num; k++){
			if(item->itemId == st->itemId[k]){
				listidx = k;
				break;
			}
		}

		if( listidx == -1 || st->amount[listidx] <= 0 || st->amount[listidx] < item->amount){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_COUNT, item->itemId );
			return;
		}
	}

	struct mail_message msg_vendor = {};
	msg_vendor.dest_id = sd->status.char_id;
	msg_vendor.zeny = 0;
	safestrncpy( msg_vendor.send_name, "<MSG>2937</MSG>", NAME_LENGTH );
	safestrncpy( msg_vendor.title, "<MSG>2943</MSG>", MAIL_TITLE_LENGTH );
	msg_vendor.status = MAIL_NEW;
	msg_vendor.type = MAIL_INBOX_NORMAL;
	msg_vendor.timestamp = time( nullptr );

	struct mail_message msg_buyer = {};
	msg_buyer.dest_id = st->vended_id;
	safestrncpy( msg_buyer.send_name, "<MSG>2937</MSG>", NAME_LENGTH );
	safestrncpy( msg_buyer.title, "<MSG>2943</MSG>", MAIL_TITLE_LENGTH );
	msg_buyer.status = MAIL_NEW;
	msg_buyer.type = MAIL_INBOX_NORMAL;
	msg_buyer.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2932</MSG>" << timestring << "\r\n";

	uint64 total_price = 0;
	for( int i = 0; i < count; i++ ){
		const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];

		int listidx = -1;
		for(int k = 0 ; k < st->vend_num; k++){
			if(item->itemId == st->itemId[k]){
				listidx = k;
				break;
			}
		}

		int index = item->index - 2; 

		if (i < ARRAYLENGTH(msg_buyer.item)) {
			memcpy(&msg_buyer.item[i],&sd->inventory.u.items_inventory[index],sizeof(struct item));
			msg_buyer.item[i].amount = item->amount;
		}

		pc_delitem(sd, index, item->amount, 0, 0, LOG_TYPE_BUYING_STORE);
		st->amount[listidx] -= item->amount;

		std::shared_ptr<item_data> id = item_db.find(item->itemId);
		stream << "\r\n<MSG>2933</MSG>" << id->name.c_str() << "\r\n";

		uint64 item_earn = (uint64)item->amount * (uint64)st->price[listidx];

		if (is_cash) {
			stream << "<MSG>2935</MSG>" << st->price[listidx] << " Cash \r\n";
			stream << "<MSG>2934</MSG>" << item->amount << "\r\n";
			stream << "<MSG>2936</MSG>" << item_earn << " Cash \r\n";
		} else {
			stream << "<MSG>2935</MSG>" << st->price[listidx] << "z \r\n";
			stream << "<MSG>2934</MSG>" << item->amount << "\r\n";
			stream << "<MSG>2936</MSG>" << item_earn << "z \r\n";
		}

		if ((uint64)msg_vendor.zeny + item_earn > (uint64)MAX_ZENY) {
			msg_vendor.zeny = MAX_ZENY;
		} else {
			msg_vendor.zeny += (int)item_earn;
		}
		total_price += item_earn;
	}
	
	int total_tax_rate = battle_config.buy_vat; 
	int is_town = 0;

	if (Sql_Query(mmysql_handle, "SELECT `tax_rate` FROM `mayor_towns` WHERE `town_map` = '%s' AND `is_active` = 1", map_mapid2mapname(st->bl.m)) == SQL_SUCCESS) {
		if (Sql_NumRows(mmysql_handle) > 0 && Sql_NextRow(mmysql_handle) == SQL_SUCCESS) {
			char* db_data; Sql_GetData(mmysql_handle, 0, &db_data, NULL);
			if (db_data) {
				int town_tax = atoi(db_data);
				if (town_tax > total_tax_rate) total_tax_rate = town_tax;
			}
			is_town = 1;
		}
		Sql_FreeResult(mmysql_handle);
	}

	int tax = 0;
	int treasury_income = 0;

	if (is_cash) {
		tax = (int)((total_price * (uint64)total_tax_rate) / 100);
		if (tax > 0) {
			treasury_income = tax * battle_config.stall_cash_to_zeny_tax_rate;
		}
	} else {
		if (total_price >= 100) {
			tax = (int)((total_price * (uint64)total_tax_rate) / 100);
			treasury_income = tax;
		}
	}

	if (tax > 0 && total_tax_rate > 0) {
		msg_vendor.zeny -= tax; 

		if (treasury_income > 0) {
			if (Sql_Query(mmysql_handle, "UPDATE `castle_money` SET `money` = `money` + %d WHERE `castle` = 0", treasury_income) != SQL_SUCCESS) {
				Sql_ShowDebug(mmysql_handle);
			}
			if (is_town) {
				if (Sql_Query(mmysql_handle, "UPDATE `mayor_towns` SET `treasury` = `treasury` + %d WHERE `town_map` = '%s'", treasury_income, map_mapid2mapname(st->bl.m)) != SQL_SUCCESS) {
					Sql_ShowDebug(mmysql_handle);
				}
			}
		}

		if (is_cash) {
			stream << "\r\n<MSG>337</MSG> " << tax << " Cash (Tax " << total_tax_rate << "%)\r\n";
		} else {
			stream << "\r\n<MSG>337</MSG> " << tax << " Zeny (Tax " << total_tax_rate << "%)\r\n";
		}
	}

	if (is_cash && msg_vendor.zeny > 0) {
		int final_cash = msg_vendor.zeny;
		msg_vendor.zeny = 0; 
		msg_vendor.item[0].nameid = 30000;
		msg_vendor.item[0].amount = final_cash;
		msg_vendor.item[0].identify = 1;
	}
	
	stream << "\0";

	safestrncpy( msg_vendor.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	intif_Mail_send( 0, &msg_vendor );

	safestrncpy( msg_buyer.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	intif_Mail_send( 0, &msg_buyer );

	bool remain_items = false;
	for( int i = 0; i < st->vend_num; i++ ){
		if(st->amount[i] > 0){
			remain_items = true;
			break;
		}
	}

	if( save_settings&CHARSAVE_VENDING ) {
		chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
		if(!remain_items){
			stall_remove(st);
		} else
			stall_buying_save(st);
	}
}

void stall_close(map_session_data* sd){
	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return sd->status.char_id == itst->vended_id;
					});

	if(itStalls != stall_db.end()){
		switch((*itStalls)->type){
			case 0:
				stall_vending_getbackitems(*itStalls);
				clif_stall_ui_close(sd,100,STALLSTORE_OK);
				break;
			case 1:
				stall_buying_getbackzeny(*itStalls);
				clif_stall_ui_close(sd,101,STALLSTORE_OK);
				break;
		}

		stall_remove(*itStalls);
	}
}

void stall_close_from_gm(uint32 vender_id){
	ShowError("stall_close from gm \n");
	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return vender_id == itst->vender_id;
					});

	if(itStalls != stall_db.end()){
		switch((*itStalls)->type){
			case 0:
				stall_vending_getbackitems(*itStalls);
				break;
			case 1:
				stall_buying_getbackzeny(*itStalls);
				break;
		}

		stall_remove(*itStalls);
	}
}

void stall_vending_save(struct s_stall_data* st){
	for(int i = 0; i < st->vend_num; i++){
		if( Sql_Query( mmysql_handle, "UPDATE `%s` SET `amount` = %d WHERE `stalls_id` = %d AND `index` = %d;",
			stalls_vending_items_table, st->items_inventory[i].amount, st->vender_id, i) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
		}
	}
}

void stall_buying_save(struct s_stall_data* st){
	for(int i = 0; i < st->vend_num; i++){
		if( Sql_Query( mmysql_handle, "UPDATE `%s` SET `amount` = %d WHERE `stalls_id` = %d AND `nameid` = %d;",
			stalls_buying_items_table, st->amount[i], st->vender_id, st->itemId[i]) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
		}
	}
}

void stall_vending_getbackitems(struct s_stall_data* st){
	if (st->vended_id >= 6000000) return;

	struct mail_message msg_vendor = {};
	msg_vendor.dest_id = st->vended_id;
	safestrncpy( msg_vendor.send_name, "Street vendor", NAME_LENGTH );
	safestrncpy( msg_vendor.title, "Stall canceled", MAIL_TITLE_LENGTH );
	msg_vendor.status = MAIL_NEW;
	msg_vendor.type = MAIL_INBOX_NORMAL;
	msg_vendor.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "Cancellation date : " << timestring << "\r\n";

	int mail_index = 0;
	for( int i = 0; i < st->vend_num; i++ ) {
		if(st->items_inventory[i].amount > 0){
			if (mail_index < ARRAYLENGTH(msg_vendor.item)) {
				memcpy(&msg_vendor.item[mail_index],&st->items_inventory[i],sizeof(struct item));
				msg_vendor.item[mail_index].amount = st->items_inventory[i].amount;
			}
			std::shared_ptr<item_data> id = item_db.find(st->items_inventory[i].nameid);
			stream << "\r\nReturn of item : " << id->name.c_str() << "\r\n";
			stream << "Amount : " << st->items_inventory[i].amount << "\r\n";
			mail_index++;
		}
	}
	stream << "\0";

	safestrncpy( msg_vendor.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	intif_Mail_send( 0, &msg_vendor );
}

void stall_buying_getbackzeny(struct s_stall_data* st){
	if (st->vended_id >= 6000000) return;

	bool is_cash = (strncmp(st->message, "[CASH]", 6) == 0);
	struct mail_message msg_buyer = {};

	msg_buyer.dest_id = st->vended_id;
	safestrncpy( msg_buyer.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_buyer.title, "<MSG>2940</MSG>", MAIL_TITLE_LENGTH );
	msg_buyer.status = MAIL_NEW;
	msg_buyer.type = MAIL_INBOX_NORMAL;
	msg_buyer.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2946</MSG> " << timestring << "\r\n";

	uint64 total_refund = 0;
	for( int i = 0; i < st->vend_num; i++ ) {
		uint64 price = (uint64)st->amount[i] * (uint64)st->price[i];
		total_refund += price;
	}

	if (total_refund > (uint64)MAX_ZENY) {
		msg_buyer.zeny = MAX_ZENY;
	} else {
		msg_buyer.zeny = (int)total_refund;
	}

	if (is_cash && msg_buyer.zeny > 0) {
		int refund_cash = msg_buyer.zeny;
		msg_buyer.zeny = 0; 
		stream << "\r\n<MSG>2941</MSG>" << refund_cash << " Cash\r\n";
		
		msg_buyer.item[0].nameid = 30000;
		msg_buyer.item[0].amount = refund_cash;
		msg_buyer.item[0].identify = 1;
	} else {
		stream << "\r\n<MSG>2941</MSG>" << msg_buyer.zeny << " z\r\n";
	}
	stream << "\0";

	safestrncpy( msg_buyer.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	intif_Mail_send( 0, &msg_buyer );
}

void stall_remove(struct s_stall_data* st){
	if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, st->vender_id ) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
	}
	switch(st->type){
		case 0:
			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, st->vender_id ) != SQL_SUCCESS ) {
				Sql_ShowDebug(mmysql_handle);
			}
			break;
		case 1:
			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, st->vender_id ) != SQL_SUCCESS ) {
				Sql_ShowDebug(mmysql_handle);
			}
			break;
	}
	if(st->timer != INVALID_TIMER)
		delete_timer(st->timer, stall_timeout);

	stall_db.erase(
	std::remove_if(stall_db.begin(), stall_db.end(), [&](s_stall_data * const & itst) {
		return st->vender_id == itst->vender_id;
	}),
	stall_db.end());

	clif_clearunit_area(&st->bl,CLR_OUTSIGHT);
	map_delblock(&st->bl);
	map_freeblock(&st->bl);
}

TIMER_FUNC (stall_timeout){
	struct s_stall_data* st = map_id2st(id);
	nullpo_ret(st);

	st->timer = INVALID_TIMER;

	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return st->vender_id == itst->vender_id;
					});

	if(itStalls != 	stall_db.end()){
		switch((*itStalls)->type){
			case 0:
				stall_vending_getbackitems(*itStalls);
				break;
			case 1:
				stall_buying_getbackzeny(*itStalls);
				break;
		}
		stall_remove(*itStalls);
	}

	return 0;
}

bool stall_searchall(map_session_data* sd, const struct s_search_store_search* s, const struct s_stall_data* st, short type)
{
	int c, slot;
	unsigned int cidx;

	if( !st->type == type ) 
		return true;

	if(st->type == 0){
		for( int j = 0; j < s->item_count; j++ ) {
			for( int i = 0; i < st->vend_num; i++ ) {
				if(st->items_inventory[i].amount > 0
					&& s->itemlist[j].itemId == st->items_inventory[i].nameid){

					if( s->min_price && s->min_price > st->price[i] ) { 
						continue;
					}

					if( s->max_price && s->max_price < st->price[i] ) { 
						continue;
					}

					if( s->card_count ) { 
						if( itemdb_isspecial(st->items_inventory[i].card[0]) ) { 
							continue;
						}
						slot = itemdb_slots(st->items_inventory[i].nameid);

						for( c = 0; c < slot && st->items_inventory[i].card[c]; c ++ ) {
							ARR_FIND( 0, s->card_count, cidx, s->cardlist[cidx].itemId == st->items_inventory[i].card[c] );
							if( cidx != s->card_count ) { 
								break;
							}
						}

						if( c == slot || !st->items_inventory[i].card[c] ) { 
							continue;
						}
					}

					if( s->search_sd->searchstore.items.size() >= (unsigned int)battle_config.searchstore_maxresults ){
						return false;
					}

					std::shared_ptr<s_search_store_info_item> ssitem = std::make_shared<s_search_store_info_item>();

					ssitem->store_id = st->vender_id;
					ssitem->account_id = st->vended_id;
					safestrncpy( ssitem->store_name, st->message, sizeof( ssitem->store_name ) );
					ssitem->nameid = st->items_inventory[i].nameid;
					ssitem->amount = st->items_inventory[i].amount;
					ssitem->price = st->price[i];
					for( int j = 0; j < MAX_SLOTS; j++ ){
						ssitem->card[j] = st->items_inventory[i].card[j];
					}
					ssitem->refine = st->items_inventory[i].refine;
					ssitem->enchantgrade = st->items_inventory[i].enchantgrade;

					s->search_sd->searchstore.items.push_back( ssitem );
				}
			}
		}
	}
	if(st->type == 1){
		for( int j = 0; j < s->item_count; j++ ) {
			for( int i = 0; i < st->vend_num; i++ ) {
				if(st->amount[i] > 0
					&& s->itemlist[j].itemId == st->itemId[i]){
					if( s->min_price && s->min_price > st->price[i] ) { 
						continue;
					}

					if( s->max_price && s->max_price < st->price[i] ) { 
						continue;
					}

					if( s->card_count ) { 
						;
					}

					if( s->search_sd->searchstore.items.size() >= (unsigned int)battle_config.searchstore_maxresults ){
						return false;
					}

					std::shared_ptr<s_search_store_info_item> ssitem = std::make_shared<s_search_store_info_item>();

					ssitem->store_id = st->vender_id;
					ssitem->account_id = st->vended_id;
					safestrncpy( ssitem->store_name, st->message, sizeof( ssitem->store_name ) );
					ssitem->nameid = st->itemId[i];
					ssitem->amount = st->amount[i];
					ssitem->price = st->price[i];
					for( int j = 0; j < MAX_SLOTS; j++ ){
						ssitem->card[j] = 0;
					}
					ssitem->refine = 0;
					ssitem->enchantgrade = 0;

					s->search_sd->searchstore.items.push_back( ssitem );
				}
			}
		}
	}

	return true;
}

TIMER_FUNC(stall_mail_queue){
	if(stall_mail_db.size() > 0){
		for (auto it = stall_mail_db.begin(); it != stall_mail_db.end(); it++){
			if (intif_Mail_send( 0, &(*it) )){
				stall_mail_db.erase(it--);
			}
		}
	}
	add_timer(gettick() + 60000, stall_mail_queue, 0, 0); 
	return 0;
}

TIMER_FUNC(stall_init){
	DBIterator *iter = NULL;
	struct s_stall_data *st = NULL;
	int i;
	std::vector<int> stall_remove_list;

	if (Sql_Query(mmysql_handle,
		"SELECT `id`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
		"`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`,"
		"`clothes_color`, `name`, `expire_time` "
		"FROM `%s` ",
		stalls_table ) != SQL_SUCCESS )
	{
		Sql_ShowDebug(mmysql_handle);
		return 1;
	}

	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		size_t len;
		char* data;
		st = NULL;
		st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
		Sql_GetData(mmysql_handle, 0, &data, NULL); st->vender_id = atoi(data);
		Sql_GetData(mmysql_handle, 1, &data, NULL); st->vended_id = atoi(data);
		st->bl.id = st->vender_id;
		Sql_GetData(mmysql_handle, 2, &data, NULL); st->type = atoi(data);
		Sql_GetData(mmysql_handle, 3, &data, NULL); st->vd.class_ = atoi(data);
		Sql_GetData(mmysql_handle, 4, &data, NULL); st->vd.sex = (data[0] == 'F') ? SEX_FEMALE : SEX_MALE;
		Sql_GetData(mmysql_handle, 5, &data, NULL); st->bl.m = atoi(data);
		Sql_GetData(mmysql_handle, 6, &data, NULL); st->bl.x = atoi(data);
		Sql_GetData(mmysql_handle, 7, &data, NULL); st->bl.y = atoi(data);
		Sql_GetData(mmysql_handle, 8, &data, &len); safestrncpy(st->message, data, zmin(len + 1, MESSAGE_SIZE));
		Sql_GetData(mmysql_handle, 9, &data, NULL); st->vd.hair_style = atoi(data);
		Sql_GetData(mmysql_handle, 10, &data, NULL); st->vd.hair_color = atoi(data);
		Sql_GetData(mmysql_handle, 11, &data, NULL); st->vd.body_style = atoi(data);
		Sql_GetData(mmysql_handle, 12, &data, NULL); st->vd.weapon = atoi(data);
		Sql_GetData(mmysql_handle, 13, &data, NULL); st->vd.shield = atoi(data);
		Sql_GetData(mmysql_handle, 14, &data, NULL); st->vd.head_top = atoi(data);
		Sql_GetData(mmysql_handle, 15, &data, NULL); st->vd.head_mid = atoi(data);
		Sql_GetData(mmysql_handle, 16, &data, NULL); st->vd.head_bottom = atoi(data);
		Sql_GetData(mmysql_handle, 17, &data, NULL); st->vd.cloth_color = atoi(data);
		Sql_GetData(mmysql_handle, 18, &data, &len); safestrncpy(st->name, data, zmin(len + 1, MESSAGE_SIZE));
		Sql_GetData(mmysql_handle, 19, &data, NULL); st->expire_time = strtoul(data, nullptr, 10);
		st->bl.type = BL_STALL;
		stall_db.push_back(st);
	}

	for (auto& itStalls : stall_db){
		int item_count = 0;

		switch(itStalls->type){
			case 0:{
				if (Sql_Query(mmysql_handle,
					"SELECT `nameid`,`amount`,`identify`,`refine`,`attribute`"
					",`card0`,`card1`,`card2`,`card3`"
					",`option_id0`,`option_val0`,`option_parm0`,`option_id1`,`option_val1`,`option_parm1`,`option_id2`,`option_val2`,`option_parm2`,`option_id3`,`option_val3`,`option_parm3`,`option_id4`,`option_val4`,`option_parm4`"
					",`expire_time`,`bound`,`unique_id`,`enchantgrade`,`price` "
					"FROM `%s` WHERE stalls_id = %d ",
					stalls_vending_items_table, itStalls->vender_id ) != SQL_SUCCESS )
				{
					Sql_ShowDebug(mmysql_handle);
					return 1;
				}

				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
					char* data;
					struct item item;
					Sql_GetData(mmysql_handle, 0, &data, NULL); item.nameid = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 1, &data, NULL); item.amount = atoi(data);
					Sql_GetData(mmysql_handle, 2, &data, NULL); item.identify = atoi(data);
					Sql_GetData(mmysql_handle, 3, &data, NULL); item.refine = atoi(data);
					Sql_GetData(mmysql_handle, 4, &data, NULL); item.attribute = atoi(data);
					for( i = 0; i < MAX_SLOTS; ++i ){
						Sql_GetData(mmysql_handle, 5+i, &data, NULL);
						item.card[i] = atoi(data);
					}
					for( i = 0; i < MAX_ITEM_RDM_OPT; ++i ) {
						Sql_GetData(mmysql_handle, 5+MAX_SLOTS+i*3, &data, NULL); item.option[i].id = atoi(data);
						Sql_GetData(mmysql_handle, 6+MAX_SLOTS+i*3, &data, NULL); item.option[i].value = atoi(data);
						Sql_GetData(mmysql_handle, 7+MAX_SLOTS+i*3, &data, NULL); item.option[i].param = atoi(data);
					}
					Sql_GetData(mmysql_handle, 5+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.expire_time = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 6+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.bound = atoi(data);
					Sql_GetData(mmysql_handle, 7+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.unique_id = strtoull(data, nullptr, 10);;
					Sql_GetData(mmysql_handle, 8+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.enchantgrade = atoi(data);

					Sql_GetData(mmysql_handle, 9+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); itStalls->price[item_count] = atoi(data);
					memcpy(&itStalls->items_inventory[item_count],&item,sizeof(struct item));
					item_count++;
				}
			} break;
			case 1: {
				if (Sql_Query(mmysql_handle,
					"SELECT `nameid`,`amount`,`price` "
					"FROM `%s` WHERE stalls_id = %d ",
					stalls_buying_items_table, itStalls->vender_id ) != SQL_SUCCESS )
				{
					Sql_ShowDebug(mmysql_handle);
					return 1;
				}

				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
					char* data;

					Sql_GetData(mmysql_handle, 0, &data, NULL); itStalls->itemId[item_count] = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 1, &data, NULL); itStalls->amount[item_count] = atoi(data);
					Sql_GetData(mmysql_handle, 2, &data, NULL); itStalls->price[item_count] = atoi(data);

					item_count++;
				}
			} break;
		}
		itStalls->vend_num = item_count;
		long int remain_time = static_cast<long int>(itStalls->expire_time - time(NULL));

		if(item_count == 0 || remain_time < 0){

			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, itStalls->vender_id ) != SQL_SUCCESS ) {
					Sql_ShowDebug(mmysql_handle);
			}

			stall_remove_list.push_back(itStalls->vender_id);

			if(remain_time < 0 && item_count > 0){
				switch(itStalls->type){
					case 0:
						stall_vending_getbackitems(itStalls);
						if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, itStalls->vender_id ) != SQL_SUCCESS ) {
								Sql_ShowDebug(mmysql_handle);
						}
						break;
					case 1:
						stall_buying_getbackzeny(itStalls);
						if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, itStalls->vender_id ) != SQL_SUCCESS ) {
								Sql_ShowDebug(mmysql_handle);
						}
						break;
				}
			}
			aFree(itStalls);
			continue;
		}

		itStalls->timer = add_timer(gettick() + (remain_time) * 1000,
			stall_timeout, itStalls->bl.id, 0);

		if(map_addblock(&itStalls->bl))
			continue;
		status_change_init(&itStalls->bl);
		map_addiddb(&itStalls->bl);
	}

	if(stall_remove_list.size() > 0){
		for (auto& itStalls : stall_remove_list){
			stall_db.erase(
			std::remove_if(stall_db.begin(), stall_db.end(), [&](s_stall_data * const & itst) {
				return itStalls == itst->vender_id;
			}),
			stall_db.end());
		}
	}
	stall_remove_list.clear();

	ShowStatus("Done loading '" CL_WHITE "%d" CL_RESET "' vending stalls.\n", stall_db.size());

	return 0;
}

void do_init_stall(void)
{
	add_timer(gettick() + 1, stall_init, 0, 0); 
	add_timer(gettick() + 60000, stall_mail_queue, 0, 0); 
}

void do_final_stall(void)
{
	if(stall_db.size() > 0){
		for (auto& i : stall_db){
			if(i != nullptr){
				aFree(i);
				i = nullptr;
			}
		}
	}

	stall_db.clear();
	stall_mail_db.clear();
}





// =========================================================
// Custom Command: เสกร้านค้า Offline Stall (ACCOUNT 6000000 FIX)
// ฉบับเพิ่มระบบ C++ Grid Validation ป้องกันยืนทับคน/NPC/บอท
// =========================================================
BUILDIN_FUNC(spawn_offline_stall) {
	const char* map_name = script_getstr(st, 2);
	int x = script_getnum(st, 3);
	int y = script_getnum(st, 4);
	int class_ = script_getnum(st, 5);
	int sex = script_getnum(st, 6);
	int hair = script_getnum(st, 7);
	int hair_color = script_getnum(st, 8);
	int clothes_color = script_getnum(st, 9);
	int head_top = script_getnum(st, 10);
	int trade_type = script_getnum(st, 11);
	const char* item_data_str = script_getstr(st, 12);
	const char* title = script_getstr(st, 13);
	const char* name = script_getstr(st, 14);

	int16 m = map_mapname2mapid(map_name);
	if (m < 0) return 0;

// ========================================================
	// 🛡️ [C++ Engine แก้ไขใหม่] ระบบสแกนหาพิกัดว่างและเว้นระยะห่างอัตโนมัติ 2 ช่อง
	// เปลี่ยนมาใช้ map_count_oncell และเช็คขอบเขตแบบ Pure C++ รันผ่านชัวร์ ไม่พึ่งพารามิเตอร์เมา ๆ
	// ========================================================
	int rx, ry;
	int target_x = x;
	int target_y = y;
	bool is_space_blocked = false;
	int search_attempts = 0;
	struct map_data *md_info = map_getmapdata(m);

	while (search_attempts < 100) { // วนหาช่องว่างปลอดภัยสูงสุด 100 รอบ
		is_space_blocked = false;
		
		// 1. เช็คพื้นผิวก่อนว่าเดินได้ไหม และไม่ใช่ช่องที่ระบบระบุว่าห้ามตั้งร้าน
		if (map_getcell(m, target_x, target_y, CELL_CHKNOPASS) || map_getcell(m, target_x, target_y, CELL_CHKNOVENDING)) {
			is_space_blocked = true;
		} else {
			// 2. วนลูปกล่องสี่เหลี่ยมสแกนรอบรัศมีจุดตั้งร้าน 2 ช่อง (กล่องขนาด 5x5 ช่อง)
			for (rx = target_x - 2; rx <= target_x + 2 && !is_space_blocked; rx++) {
				for (ry = target_y - 2; ry <= target_y + 2; ry++) {
					// สแกนหาว่ามีผู้เล่นจริง (BL_PC) กำลังกางป้ายขาย/ซื้อของอยู่บนเซลล์นั้นไหม
					if (map_count_oncell(m, rx, ry, BL_PC, 1) > 0) {
						is_space_blocked = true;
						break;
					}
					// สแกนหาว่ามี NPC หรือ บอทออฟไลน์ตัวเก่า (BL_STALL) ตั้งขวางอยู่ไหม
					if (map_count_oncell(m, rx, ry, BL_NPC | BL_STALL, 0) > 0) {
						is_space_blocked = true;
						break;
					}
				}
			}
		}
		
		// ถ้าพื้นที่เคลียร์ ใสสะอาด ไม่มีอะไรบัง ยอมรับพิกัดนี้ทันที!
		if (!is_space_blocked) {
			x = target_x;
			y = target_y;
			break;
		}
		
		// 3. ถ้าจุดเดิมติดบล็อก ให้สุ่มขยับพิกัดหาช่องใหม่รอบๆ ตัว
		if (md_info != nullptr) {
			int next_x = x + (rnd() % 5 - 2);
			int next_y = y + (rnd() % 5 - 2);
			
			// ใช้ลอจิก Pure C++ คุมขอบเขตพิกัดแทน cap_value ป้องกัน Error ตัวแปรสูญหาย
			if (next_x < 0) next_x = 0;
			if (next_x >= md_info->xs) next_x = md_info->xs - 1;
			if (next_y < 0) next_y = 0;
			if (next_y >= md_info->ys) next_y = md_info->ys - 1;
			
			target_x = next_x;
			target_y = next_y;
		}
		search_attempts++;
	}

	// กรณีแมพเต็มจริงๆ หาที่ลงไม่ได้เลย ให้ข้ามการสร้างบอทตัวนี้ไปเพื่อป้องกันลูปนรก
	if (is_space_blocked && search_attempts >= 100) {
		return 0;
	}
	// ========================================================
	struct s_stall_data *st_stall = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
	st_stall->vender_id = stall_getuid();
	
	// 🌟 THE MASTER KEY: ใช้ฐาน ID 6,000,000 ให้ตรงกับระบบ Core เซิร์ฟเวอร์!
	st_stall->vended_id = 6000000 + st_stall->vender_id; 
	
	st_stall->type = trade_type; 
	st_stall->expire_time = (unsigned int)(time(NULL) + 86400);
	
	safestrncpy(st_stall->message, title, MESSAGE_SIZE);
	safestrncpy(st_stall->name, name, NAME_LENGTH);

	st_stall->bl.id = st_stall->vender_id;
	st_stall->bl.type = BL_STALL;
	st_stall->bl.m = m;
	st_stall->bl.x = x;
	st_stall->bl.y = y;

	st_stall->vd.class_ = class_;
	st_stall->vd.sex = (sex == 0) ? SEX_FEMALE : SEX_MALE;
	st_stall->vd.hair_style = hair;
	st_stall->vd.hair_color = hair_color;
	st_stall->vd.cloth_color = clothes_color;
	st_stall->vd.head_top = head_top;

	char message_sql[MESSAGE_SIZE*2];
	Sql_EscapeString(mmysql_handle, message_sql, st_stall->message);

	// 🛠️ ยัดบอทเข้า Account ID 6000000 เพื่อปลดล็อคการส่งเงินและไอเทมข้าม SQL
	Sql_Query(mmysql_handle, "SET FOREIGN_KEY_CHECKS=0;");
	char bot_char_name[24];
	sprintf(bot_char_name, "Bot_%d", st_stall->vended_id);
	Sql_Query(mmysql_handle, "INSERT IGNORE INTO `char` (`account_id`, `char_id`, `char_num`, `name`, `class`, `zeny`) VALUES (6000000, %d, 0, '%s', 24, 2000000000)", st_stall->vended_id, bot_char_name);
	Sql_Query(mmysql_handle, "UPDATE `char` SET `zeny` = 2000000000 WHERE `char_id` = %d", st_stall->vended_id);
	Sql_Query(mmysql_handle, "SET FOREIGN_KEY_CHECKS=1;");

	Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
		"`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`,"
		"`clothes_color`, `name`, `expire_time`) "
		"VALUES( %d, %d, %d, %d, '%c', '%d', %d, %d, '%s', %d, %d, 0, 0, 0, %d, 0, 0, %d, '%s', %u  );",
		stalls_table, st_stall->vender_id, st_stall->vended_id, st_stall->type, st_stall->vd.class_, st_stall->vd.sex == SEX_FEMALE ? 'F' : 'M', st_stall->bl.m, st_stall->bl.x, st_stall->bl.y,
		message_sql, st_stall->vd.hair_style, st_stall->vd.hair_color, st_stall->vd.head_top,
		st_stall->vd.cloth_color, st_stall->name, st_stall->expire_time);

	char item_data[1024];
	safestrncpy(item_data, item_data_str, sizeof(item_data));
	int slot = 0;
	char* token = strtok(item_data, ":");

	while (token != NULL && slot < 10) { 
		int i_id = 0, i_amt = 0, i_price = 0;
		if (sscanf(token, " %d , %d , %d ", &i_id, &i_amt, &i_price) == 3) {
			
			bool is_duplicate = false;
			for(int check = 0; check < slot; check++) {
				if (trade_type == 0 && st_stall->items_inventory[check].nameid == i_id) is_duplicate = true;
				if (trade_type == 1 && st_stall->itemId[check] == i_id) is_duplicate = true;
			}
			
			if (!is_duplicate) {
				if (trade_type == 0) { 
					st_stall->items_inventory[slot].nameid = i_id;
					st_stall->items_inventory[slot].amount = i_amt;
					st_stall->items_inventory[slot].identify = 1;
					st_stall->price[slot] = i_price;
					
					Sql_Query(mmysql_handle, "INSERT INTO `%s`(`stalls_id`,`index`,`nameid`,`amount`,`identify`,`price`) VALUES (%d, %d, %d, %d, 1, %d)",
						stalls_vending_items_table, st_stall->vender_id, slot, i_id, i_amt, i_price);
				} else { 
					st_stall->itemId[slot] = i_id;
					st_stall->amount[slot] = i_amt;
					st_stall->price[slot] = i_price;
					
					Sql_Query(mmysql_handle, "INSERT INTO `%s`(`stalls_id`,`nameid`,`amount`,`price`) VALUES (%d, %d, %d, %d)",
						stalls_buying_items_table, st_stall->vender_id, i_id, i_amt, i_price);
				}
				slot++;
			}
		}
		token = strtok(NULL, ":");
	}

	st_stall->vend_num = slot; 
	st_stall->timer = add_timer(gettick() + 86400 * 1000, stall_timeout, st_stall->bl.id, 0);

	if(map_addblock(&st_stall->bl)) return 0;
	status_change_init(&st_stall->bl);
	map_addiddb(&st_stall->bl);
	stall_db.push_back(st_stall);

	return 0;
}

// =========================================================
// Custom Command: ล้างร้าน Offline Stall ของระบบ
// =========================================================
BUILDIN_FUNC(clear_offline_stall) {
	std::vector<int> remove_list;
	for (auto& itStalls : stall_db) {
		if (itStalls->vended_id >= 6000000) remove_list.push_back(itStalls->vender_id);
	}
	for (auto& vid : remove_list) {
		auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
			return vid == itst->vender_id;
		});
		if(itStalls != stall_db.end()){
			Sql_Query( mmysql_handle, "DELETE FROM `char` WHERE `char_id` = %d AND `account_id` = 6000000;", (*itStalls)->vended_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, (*itStalls)->vender_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, (*itStalls)->vender_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, (*itStalls)->vender_id );
			if((*itStalls)->timer != INVALID_TIMER) delete_timer((*itStalls)->timer, stall_timeout);
			clif_clearunit_area(&(*itStalls)->bl, CLR_OUTSIGHT);
			map_delblock(&(*itStalls)->bl);
			map_freeblock(&(*itStalls)->bl);
			stall_db.erase(itStalls);
		}
	}
	return 0;
}

BUILDIN_FUNC(clear_offline_stall_type) {
	int clear_type = script_getnum(st, 2);
	if (clear_type != 0 && clear_type != 1)
		return 0;

	std::vector<int> remove_list;
	for (auto& itStalls : stall_db) {
		if (itStalls != nullptr && itStalls->vended_id >= 6000000 && itStalls->type == clear_type)
			remove_list.push_back(itStalls->vender_id);
	}

	for (auto& vid : remove_list) {
		auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
			return itst != nullptr && vid == itst->vender_id;
		});
		if(itStalls != stall_db.end()){
			Sql_Query( mmysql_handle, "DELETE FROM `char` WHERE `char_id` = %d AND `account_id` = 6000000;", (*itStalls)->vended_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, (*itStalls)->vender_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, (*itStalls)->vender_id );
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, (*itStalls)->vender_id );
			if((*itStalls)->timer != INVALID_TIMER) delete_timer((*itStalls)->timer, stall_timeout);
			clif_clearunit_area(&(*itStalls)->bl, CLR_OUTSIGHT);
			map_delblock(&(*itStalls)->bl);
			map_deliddb(&(*itStalls)->bl);
			status_change_clear(&(*itStalls)->bl, 1);
			map_freeblock(&(*itStalls)->bl);
			stall_db.erase(itStalls);
		}
	}
	return 0;
}
