#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// 起動してからの総ティック数（1秒 = 100ティック）
extern volatile uint32_t timer_ticks;

// PITタイマーを初期化（100Hz = 0.01秒ごとに割り込み）
void init_timer(uint32_t frequency);

// 割り込みハンドラから呼ばれる関数
void timer_tick(void);

// 起動してからの経過秒数を取得
uint32_t get_uptime_seconds(void);

// メニューバー表示用の時間文字列 ("00:00:00") を作成
void format_uptime(char *buffer);

#endif
