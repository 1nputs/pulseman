/* The I/O chip's controller ports ($A10001-$A1001F): the version register, three data and three
 * control registers, and the pads on ports 1 and 2. The platform sets the buttons held; the model
 * answers the game's reads. */
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

/* after each access (a recorder): a write (1) or read (0), the address and the value */
typedef void (*mdrt_pads_access_fn)(void *user, int write, uint32_t addr, uint8_t value);

typedef struct mdrt_pads {
    uint8_t reg[16];            /* the I/O registers by (address - $A10001) / 2 */
    uint16_t buttons[2];
    uint8_t type[2];
    uint8_t steps[2];           /* 6-button: TH transitions since the counter last reset */
    mdrt_pads_access_fn observe;
    void *observe_user;
} mdrt_pads;

/* version register: overseas (bit 7), PAL (bit 6) */
void mdrt_pads_init(mdrt_pads *p, int overseas, int pal);
void mdrt_pad_set_type(mdrt_pads *p, int port, int type);
void mdrt_pad_set(mdrt_pads *p, int port, uint16_t buttons);
/* a byte access to $A10000-$A1001F */
uint8_t mdrt_pad_read(mdrt_pads *p, uint32_t addr);
void mdrt_pad_write(mdrt_pads *p, uint32_t addr, uint8_t value);
/* a frame boundary: the 6-button pad's TH counter times out (it resets after about 1.5 ms
 * without a transition, and games read the pads once a frame) */
void mdrt_pads_frame(mdrt_pads *p);
void mdrt_pads_set_observer(mdrt_pads *p, mdrt_pads_access_fn fn, void *user);
/* the I/O chip and the pads as bytes: save returns the size (out NULL: only that); load returns
 * 0, or -1 for the wrong size (then nothing changed) */
size_t mdrt_pads_save(const mdrt_pads *p, uint8_t *out);
int mdrt_pads_load(mdrt_pads *p, const uint8_t *in, size_t size);

#endif
