// memory/homeostasis.c
#include "homeostasis.h"

const HomeostasisConfig HOMEOSTASIS_DEFAULT = {
    .target_load    = 0.70f,
    .eta_threshold  = 0.05f,
    .eta_decay      = 0.02f,
    .threshold_min  = 0.30f,
    .threshold_max  = 0.95f,
    .decay_floor    = 0.75f,
    .sweep_seconds  = 3600,   // весь архив просканирован не реже раза в час
    .tick_seconds   = 10,     // совпадает с decay_timer_loop() в subconscious.c
};

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void homeostasis_init(HomeostasisState *st) {
    if (!st) return;
    st->activation_threshold = 0.60f;   // стартовая точка = прежний хардкод, но теперь дрейфует
    st->policy = DECAY_POLICY_DEFAULT;  // стартовая политика, дальше эволюционирует
}

void homeostasis_step(HomeostasisState *st, const HomeostasisConfig *cfg,
                       WorkingMemory *wm, uint64_t total_atom_count) {
    if (!st || !cfg || !wm) return;

    wm_rdlock(wm);
    float load = wm->capacity ? (float)wm->count / (float)wm->capacity : 0.0f;
    wm_unlock(wm);

    float error = load - cfg->target_load;

    // Allostatic loop: перегрузка WM -> строже порог отбора целей + быстрее decay
    st->activation_threshold = clampf(
        st->activation_threshold + cfg->eta_threshold * error,
        cfg->threshold_min, cfg->threshold_max);

    st->policy.sti_decay_factor = clampf(
        st->policy.sti_decay_factor - cfg->eta_decay * error,
        cfg->decay_floor, 1.0f);

    float sti_delta = 1.0f - st->policy.sti_decay_factor;
    st->policy.truth_conf_decay   = clampf(1.0f - sti_delta * 0.5f, 0.90f, 0.999f);
    st->policy.valence_regression = clampf(sti_delta * 0.5f, 0.01f, 0.20f);

    // batch_size масштабируется под реальный размер базы, гарантируя
    // полный проход архива за sweep_seconds вместо фиксированных 2048.
    uint64_t ticks_per_sweep = cfg->sweep_seconds / (cfg->tick_seconds ? cfg->tick_seconds : 1);
    if (ticks_per_sweep == 0) ticks_per_sweep = 1;
    uint64_t needed = total_atom_count / ticks_per_sweep;
    if (needed < 256)     needed = 256;
    if (needed > 200000)  needed = 200000; // защита от одного гигантского write-lock'а
    st->policy.batch_size = (uint32_t)needed;
}