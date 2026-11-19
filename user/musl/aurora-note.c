// Nota ELF que marca un binario como app nativa de AuroraOS (la lee aurora-run
// para no tratarlo como una app Linux). El Makefile la enlaza en cada app de
// user/musl/apps con -Wl,-u,aurora_app_note para que --gc-sections no la quite.
#include <stdint.h>
struct aurora_note {
  uint32_t namesz, descsz, type;
  char name[8];
  uint32_t desc; // version del formato de app de Aurora
};
__attribute__((section(".note.aurora"), aligned(4), used))
const struct aurora_note aurora_app_note = {7, 4, 1, "Aurora", 1};
