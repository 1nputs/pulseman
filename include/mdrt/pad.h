/* The I/O chip's controller ports ($A10001-$A1001F): the version register, three data and three
 * control registers, and the pads on ports 1 and 2, or four pads through an adaptor plugged into
 * both. The platform sets the buttons held; the model answers the game's reads.
 *
 * EA's 4 Way Play (MDRT_ADAPTOR_EA4WAY): port 2's lines D6-D4, as the game drives them (a line
 * that is not an output counts as 0), choose what port 1 reads. D6 low: pad D5-D4 (0-3), a 3-button
 * pad under port 1's TH. D6 high: the adaptor itself, D1-D0 low and D5-D2 high (games write $7C to
 * port 2 and look for D1-D0 clear on port 1 to find it). Port 2 reads nothing. As the reference
 * emulator answers a game that drives it (a .padtrace of device 1). */
#ifndef MDRT_PAD_H
#define MDRT_PAD_H
#include <stddef.h>
#include <stdint.h>

/* buttons, in the bit order the 3-button protocol returns them */
enum {
    MDRT_PAD_UP = 0x001, MDRT_PAD_DOWN = 0x002, MDRT_PAD_LEFT = 0x004, MDRT_PAD_RIGHT = 0x008,
    MDRT_PAD_B = 0x010, MDRT_PAD_C = 0x020, MDRT_PAD_A = 0x040, MDRT_PAD_START = 0x080,
    MDRT_PAD_Z = 0x100, MDRT_PAD_Y = 0x200, MDRT_PAD_X = 0x400, MDRT_PAD_MODE = 0x800
};
enum { MDRT_PAD_NONE, MDRT_PAD_3BUTTON, MDRT_PAD_6BUTTON };
enum { MDRT_ADAPTOR_NONE, MDRT_ADAPTOR_EA4WAY };
enum { MDRT_PADS = 4 };

/* after each access (a recorder): a write (1) or read (0), the address and the value */
typedef void (*mdrt_pads_access_fn)(void *user, int write, uint32_t addr, uint8_t value);

typedef struct mdrt_pads {
    uint8_t reg[16];            /* the I/O registers by (address - $A10001) / 2 */
    uint16_t buttons[MDRT_PADS];   /* pads 1-4: 3 and 4 only through an adaptor */
    uint8_t type[2];
    uint8_t steps[2];           /* 6-button: TH transitions since the counter last reset */
    uint8_t adaptor;            /* MDRT_ADAPTOR_* in the ports (none: a pad in each) */
    mdrt_pads_access_fn observe;
    void *observe_user;
} mdrt_pads;

/* version register: overseas (bit 7), PAL (bit 6) */
void mdrt_pads_init(mdrt_pads *p, int overseas, int pal);
void mdrt_pad_set_type(mdrt_pads *p, int port, int type);
/* the buttons held on pad 0-3 (2 and 3: an adaptor's third and fourth) */
void mdrt_pad_set(mdrt_pads *p, int pad, uint16_t buttons);
/* what is plugged into both ports (MDRT_ADAPTOR_NONE: a pad in each) */
void mdrt_pads_set_adaptor(mdrt_pads *p, int adaptor);
/* a byte access to $A10000-$A1001F */
uint8_t mdrt_pad_read(mdrt_pads *p, uint32_t addr);
void mdrt_pad_write(mdrt_pads *p, uint32_t addr, uint8_t value);
/* a frame boundary: the 6-button pad's TH counter times out (it resets after about 1.5 ms
 * without a transition, and games read the pads once a frame) */
void mdrt_pads_frame(mdrt_pads *p);
void mdrt_pads_set_observer(mdrt_pads *p, mdrt_pads_access_fn fn, void *user);
/* the I/O chip and the pads as bytes: save returns the size (out NULL: only that); load returns
 * 0, or -1 for the wrong size (then nothing changed). A state from before the adaptor (two pads)
 * loads too, with pads 3 and 4 up and no adaptor. */
size_t mdrt_pads_save(const mdrt_pads *p, uint8_t *out);
int mdrt_pads_load(mdrt_pads *p, const uint8_t *in, size_t size);

#endif
