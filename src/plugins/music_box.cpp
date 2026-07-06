#include "map.hpp"
#include "pc.hpp"
#include "clif.hpp"
#include "script.hpp"
#include "timer.hpp"  // แก้เป็น .hpp
#include "sql.hpp"    // แก้เป็น .hpp

#define STAMINA_INCREASE_INTERVAL 10000 // 10 seconds
#define STAMINA_INCREASE_AMOUNT 5
#define MUSIC_BOX_RANGE 10 // Range within which players gain stamina

static void update_stamina_in_db(int account_id, int stamina_increase) {
    char query[256];
    snprintf(query, sizeof(query), "UPDATE `stamina` SET `Boxstamina_plus` = `Boxstamina_plus` + %d WHERE `account_id` = %d", stamina_increase, account_id);
    if (SQL_ERROR == Sql_Query(sql_handle, query)) {
        Sql_ShowDebug(sql_handle);
    }
}

static int is_valid_player(struct map_session_data* sd) {
    return sd && sd->status.account_id > 0 && sd->state.auth;
}

static void music_box_stamina_increase(int tid, int64 tick, int id, intptr_t data) {
    struct map_session_data* sd = map_id2sd(id);
    if (!is_valid_player(sd)) {
        return; // Stop the timer if the player is invalid
    }

    struct block_list* bl = map_id2bl(id);
    if (!bl) {
        return;
    }

    map_foreachinrange(
        (void(*)(struct block_list*, va_list))[] (struct block_list* bl, va_list ap) {
            struct map_session_data* tsd = (struct map_session_data*)bl;
            if (is_valid_player(tsd) && tsd->status.stamina < MAX_STAMINA) {
                tsd->status.stamina += STAMINA_INCREASE_AMOUNT;
                clif_updatestatus(tsd, SP_STAMINA);
                update_stamina_in_db(tsd->status.account_id, STAMINA_INCREASE_AMOUNT);
            }
        }, bl, MUSIC_BOX_RANGE, BL_PC
    );

    add_timer(gettick() + STAMINA_INCREASE_INTERVAL, music_box_stamina_increase, id, 0);
}

void clif_play_custom_bgm(struct map_session_data* sd, const char* bgm_name) {
    if (!is_valid_player(sd) || !bgm_name) return;

    size_t packet_len = 6 + strlen(bgm_name) + 1;
    WFIFOHEAD(sd->fd, packet_len);
    WFIFOW(sd->fd, 0) = 0x2b9; // Custom Packet ID (ensure this is not conflicting)
    WFIFOL(sd->fd, 2) = strlen(bgm_name) + 1;
    strcpy(WFIFOP(sd->fd, 6), bgm_name);
    WFIFOSET(sd->fd, packet_len);

    add_timer(gettick() + STAMINA_INCREASE_INTERVAL, music_box_stamina_increase, sd->bl.id, 0);
}

BUILDIN_FUNC(play_music_box) {
    struct map_session_data* sd = script_rid2sd(st);
    const char* bgm_name = script_getstr(st, 2);

    if (!is_valid_player(sd) || !bgm_name) {
        return SCRIPT_CMD_FAILURE;
    }

    clif_play_custom_bgm(sd, bgm_name);

    return SCRIPT_CMD_SUCCESS;
}

// Register the custom script command
void music_box_script_register(void) {
    add_buildin("play_music_box", play_music_box);
}

// Entry point for the plugin
int plugin_init(void) {
    music_box_script_register();
    return 0;
}

int plugin_final(void) {
    return 0;
}
