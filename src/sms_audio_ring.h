#ifndef SMS_AUDIO_RING_H
#define SMS_AUDIO_RING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef IRAM_ATTR
#define SMS_AUDIO_ISR_ATTR IRAM_ATTR
#else
#define SMS_AUDIO_ISR_ATTR
#endif

/* Fila SPSC: emulador produz quadros PCM; ISR do DAC consome cada amostra.
 * A ISR NUNCA acessa snd.buffer[] enquanto o SN76496 o esta escrevendo.
 * A publicacao do indice apos a copia completa preserva quadros inteiros.
 * 2048 amostras = 130 ms a 15720 Hz. Nao usar PSRAM no caminho da ISR.
 * reset() somente com a saida desabilitada (snd.enabled == 0).
 */
class SMSAudioRing {
public:
    static const uint32_t CAPACITY = 2048u;
    SMSAudioRing() { reset(); }

    void reset() {
        __atomic_store_n(&read_, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&write_, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&underflows_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&overflows_, 0u, __ATOMIC_RELAXED);
    }

    bool pushStereo(const int16_t* left, const int16_t* right, uint32_t count) {
        if (!left || !right || count == 0u || count > CAPACITY) return false;
        const uint32_t r = __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
        const uint32_t w = __atomic_load_n(&write_, __ATOMIC_RELAXED);
        if ((w - r) > CAPACITY || count > CAPACITY - (w - r)) {
            __atomic_fetch_add(&overflows_, 1u, __ATOMIC_RELAXED);
            return false; /* nao publicar amostras parciais */
        }
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t mixed = ((int32_t)left[i] + (int32_t)right[i]) >> 1;
            pcm_[(w + i) & (CAPACITY - 1u)] = (int16_t)mixed;
        }
        __atomic_store_n(&write_, w + count, __ATOMIC_RELEASE);
        return true;
    }

    /* NES output is MONO.  The old pushStereo(pcm,pcm) mixed every sample
     * with itself; a two-segment copy is identical and avoids per-sample
     * add/shift and index masking.  Publication stays one RELEASE store.
     * Existing SMS stereo path is unchanged. */
    bool pushMono(const int16_t* mono, uint32_t count) {
        if (!mono || count == 0u || count > CAPACITY) return false;
        const uint32_t r = __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
        const uint32_t w = __atomic_load_n(&write_, __ATOMIC_RELAXED);
        if ((w - r) > CAPACITY || count > CAPACITY - (w - r)) {
            __atomic_fetch_add(&overflows_, 1u, __ATOMIC_RELAXED);
            return false;
        }
        const uint32_t first = (count < (CAPACITY - (w & (CAPACITY - 1u))))
            ? count : (CAPACITY - (w & (CAPACITY - 1u)));
        memcpy(pcm_ + (w & (CAPACITY - 1u)), mono, first * sizeof(int16_t));
        if (first < count)
            memcpy(pcm_, mono + first, (count - first) * sizeof(int16_t));
        __atomic_store_n(&write_, w + count, __ATOMIC_RELEASE);
        return true;
    }

    /* Somente no contexto da ISR de audio. */
    SMS_AUDIO_ISR_ATTR bool pop(int16_t* sample) {
        const uint32_t r = __atomic_load_n(&read_, __ATOMIC_RELAXED);
        const uint32_t w = __atomic_load_n(&write_, __ATOMIC_ACQUIRE);
        if (r == w) {
            __atomic_fetch_add(&underflows_, 1u, __ATOMIC_RELAXED);
            return false;
        }
        *sample = pcm_[r & (CAPACITY - 1u)];
        __atomic_store_n(&read_, r + 1u, __ATOMIC_RELEASE);
        return true;
    }

    SMS_AUDIO_ISR_ATTR uint32_t available() const {
        const uint32_t w = __atomic_load_n(&write_, __ATOMIC_ACQUIRE);
        const uint32_t r = __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
        return w - r;
    }
    uint32_t underflows() const {
        return __atomic_load_n(&underflows_, __ATOMIC_RELAXED);
    }
    uint32_t overflows() const {
        return __atomic_load_n(&overflows_, __ATOMIC_RELAXED);
    }

private:
    int16_t pcm_[CAPACITY];
    uint32_t read_ = 0, write_ = 0, underflows_ = 0, overflows_ = 0;
};

#undef SMS_AUDIO_ISR_ATTR
#endif
