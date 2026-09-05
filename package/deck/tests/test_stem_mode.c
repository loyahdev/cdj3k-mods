#include "test.h"
#include "mods/stem/mode.h"

static void check(int server_in, int prestem_in,
                  enum stem_mode_preference preference,
                  int server_want, int prestem_want)
{
    int server = server_in;
    int prestem = prestem_in;

    stem_mode_exclusive(&server, &prestem, preference);
    CHECK_INT(server, server_want);
    CHECK_INT(prestem, prestem_want);
}

int main(void)
{
    check(0, 0, STEM_MODE_PREFER_SERVER, 0, 0);
    check(1, 0, STEM_MODE_PREFER_SERVER, 1, 0);
    check(0, 1, STEM_MODE_PREFER_PRESTEM, 0, 1);
    check(1, 1, STEM_MODE_PREFER_SERVER, 1, 0);
    check(1, 1, STEM_MODE_PREFER_PRESTEM, 0, 1);
    check(7, -2, STEM_MODE_PREFER_SERVER, 1, 0);
    check(7, -2, STEM_MODE_PREFER_PRESTEM, 0, 1);
    return t_done("stem_mode");
}
