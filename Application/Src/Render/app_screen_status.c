/**
 * @file    app_screen_status.c
 * @brief   整屏门面 · 卡片状态与整屏状态快照实现
 *
 * 由 app_screen.c 拆出（簇 B）：状态本属整屏门面 —— 它描述"这张卡在不在"，
 * 与用哪种总线、哪个协议探活无关；协议只**驱动**它，不拥有它。
 *
 * 卡表（`s_card_table`）与几何/身份仍留在 app_screen.c（簇 A/F）—— 本文件只经
 * 公开 API `app_screen_card()` 读、经内部接缝 `app_screen_card_state_store()` 写，
 * 于是这里自己的 static（告警回调与三个计数器）全归自己，无需 extern 共享变量。
 */

#include "app_screen.h"

/* 最近一轮序号：**只存低 8 位**，只为日志对照，不参与任何判断 */
static uint8_t s_status_seq_lo;

static app_screen_alarm_fn_t s_alarm_fn;
static uint16_t              s_retrans_cnt;
static uint8_t               s_evict_cnt;
static uint8_t               s_last_alarm;

app_screen_card_state_t app_screen_card_state(uint8_t card_idx)
{
    const app_screen_card_t *c = app_screen_card(card_idx);
    return c ? (app_screen_card_state_t)c->state : APP_SCREEN_CARD_STATE_MISSING;
}

void app_screen_card_set_state(uint8_t card_idx, app_screen_card_state_t st)
{
    /* 真正写卡表的是内部接缝（卡表属 app_screen.c）；状态没变就返回，不算一次跳变 */
    uint8_t addr = 0;
    if (!app_screen_card_state_store(card_idx, st, &addr)) return;

    if (st == APP_SCREEN_CARD_STATE_OFFLINE) s_evict_cnt++;
    s_last_alarm = addr;

    /* **不注册就什么都不发生** —— 协议只暴露状态，不决定去向，也不认识任何上层协议。
       要报的产品自己注册；不报的产品一个字节都不产生。 */
    if (s_alarm_fn) s_alarm_fn(addr, (uint8_t)st);
}

void app_screen_register_alarm(app_screen_alarm_fn_t fn)
{
    s_alarm_fn = fn;
}

void app_screen_note_retrans(void)
{
    s_retrans_cnt++;
}

void app_screen_note_round(uint16_t seq)
{
    s_status_seq_lo = (uint8_t)seq;
}

void app_screen_status(app_screen_status_t *out)
{
    if (!out) return;

    out->seq_lo      = s_status_seq_lo;
    out->online_mask = 0;
    out->retrans_cnt = s_retrans_cnt;
    out->evict_cnt   = s_evict_cnt;
    out->last_alarm  = s_last_alarm;

    /* 位 i ↔ 地址 i+1：地址 0 是本卡，不进掩码。从卡地址上限 0x1F，8 位够放
       （本工程 APP_SCREEN_CARD_MAX=4，实际只用到低 3 位）。 */
    const app_screen_layout_t *tbl = app_screen_layout();
    for (uint8_t i = 0; i < tbl->count; i++) {
        const app_screen_card_t *c = app_screen_card(i);
        if (c && c->addr >= 1U && c->addr <= 8U && c->state == (uint8_t)APP_SCREEN_CARD_STATE_ONLINE)
            out->online_mask |= (uint8_t)(1U << (c->addr - 1U));
    }
}
