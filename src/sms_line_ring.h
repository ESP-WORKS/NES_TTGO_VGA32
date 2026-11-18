// sms_line_ring.h - anel SPSC de linhas VGA (v0.1.34)
//
// Produtor (emulador, core 1) empacota cada linha direto num slot e a
// publica; consumidor (tarefa no core 0) copia slot->dst (PSRAM). Sem
// FreeRTOS aqui: acordar/ceder CPU sao callbacks, o que permite testar a
// mesma logica no host com threads (tests/test_sms_line_ring.cpp).
//
// Regras de memoria: head so e escrito pelo produtor, tail so pelo
// consumidor; indices monotonicos de 32 bits; publicacao com RELEASE e
// leitura com ACQUIRE. O produtor jamais reusa um slot ainda nao copiado.
#ifndef SMS_LINE_RING_H
#define SMS_LINE_RING_H
#include <stdint.h>
#include <string.h>

struct SMSLineSlot {
    uint32_t words[64];   // ate 256 pixels VGA ja empacotados (XOR2)
    uint8_t* dst;         // destino no stage PSRAM
    uint32_t len;         // bytes validos (multiplo de 4, <= 256)
};

class SMSLineRing {
public:
    typedef void (*WakeFn)(void* ctx);   // acorda o consumidor
    typedef void (*IdleFn)(void* ctx);   // cede CPU apos muita espera

    void attach(SMSLineSlot* slots, uint32_t slot_count, uint32_t batch) {
        slots_ = slots; mask_ = slot_count - 1u; count_ = slot_count;
        batch_ = batch ? batch : 1u;
        __atomic_store_n(&head_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&tail_, 0u, __ATOMIC_RELAXED);
        notified_ = 0u;
        stalls_ = 0u;
    }
    bool attached() const { return slots_ != nullptr; }
    uint32_t stalls() const { return stalls_; }
    SMSLineSlot* slots() const { return slots_; }

    // ---- produtor ----
    // Devolve o buffer do proximo slot; espera se o anel estiver cheio.
    uint32_t* acquire(WakeFn wake, void* ctx, IdleFn idle) {
        const uint32_t h = head_;
        uint32_t spins = 0;
        bool counted = false;
        while ((uint32_t)(h - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE)) >= count_) {
            if (!counted) { counted = true; ++stalls_; }   // produtor esperou o consumidor
            if (notified_ != h) { notified_ = h; wake(ctx); }  // nunca esperar sem acordar
            if (++spins > 4000u) { spins = 0; idle(ctx); }
        }
        return slots_[h & mask_].words;
    }
    // Publica o slot obtido em acquire(). flush = acordar sem esperar o lote.
    void publish(uint8_t* dst, uint32_t len, bool flush, WakeFn wake, void* ctx) {
        const uint32_t h = head_;
        SMSLineSlot* s = &slots_[h & mask_];
        s->dst = dst; s->len = len;
        const uint32_t nh = h + 1u;
        __atomic_store_n(&head_, nh, __ATOMIC_RELEASE);
        if (flush || (uint32_t)(nh - notified_) >= batch_) { notified_ = nh; wake(ctx); }
    }
    // Espera o consumidor copiar tudo o que foi publicado.
    void drain(WakeFn wake, void* ctx, IdleFn idle) {
        const uint32_t h = head_;
        if (notified_ != h) { notified_ = h; wake(ctx); }
        uint32_t spins = 0;
        while (__atomic_load_n(&tail_, __ATOMIC_ACQUIRE) != h)
            if (++spins > 4000u) { spins = 0; idle(ctx); }
    }

    // ---- consumidor ---- (devolve quantas linhas copiou)
    uint32_t consume() {
        uint32_t n = 0;
        uint32_t t = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        for (;;) {
            const uint32_t h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
            if (t == h) return n;
            while (t != h) {
                const SMSLineSlot* s = &slots_[t & mask_];
                memcpy(s->dst, s->words, s->len);
                ++t; ++n;
                __atomic_store_n(&tail_, t, __ATOMIC_RELEASE);
            }
        }
    }

private:
    SMSLineSlot* slots_ = nullptr;
    uint32_t mask_ = 0, count_ = 0, batch_ = 1;
    volatile uint32_t head_ = 0, tail_ = 0;
    uint32_t notified_ = 0;   // lado do produtor
    uint32_t stalls_ = 0;     // vezes que acquire() encontrou o anel cheio
};
#endif
