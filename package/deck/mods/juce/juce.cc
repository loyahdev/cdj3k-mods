// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * juce.cc - the JUCE operations that are more than a single call. Everything
 * else stays a macro in juce.h.
 */
#include "juce/juce.hh"

void juce_comp_repaint(uintptr_t comp)
{
    int32_t r[4];

    if (!comp || !FN_COMP_REPAINT_RECT || juce_comp_bounds(comp, r) != 0) return;
    r[0] = 0;
    r[1] = 0;
    ep_call(void(void *, const int32_t *))::at(FN_COMP_REPAINT_RECT, (void *)comp, r);
}

void juce_comp_set_visible(uintptr_t comp, int visible)
{
    uintptr_t vt = 0, fn = 0;

    if (!comp || mod_safe_read(comp, &vt, sizeof(vt)) != 0 || !vt)
        return;
    if (mod_safe_read(vt + JUCE_VT_SETVISIBLE, &fn, sizeof(fn)) != 0 || !fn)
        return;
    ep_call(void(void *, int))::at(fn, (void *)comp, visible);
}

int juce_string_read(uintptr_t str, char *buf, size_t cap)
{
    uintptr_t p = 0;
    size_t n;

    if (cap < 2 || mod_safe_read(str, &p, sizeof(p)) != 0 || !p)
        return -1;
    for (n = cap - 1; n > 0; n /= 2)
        if (mod_safe_read(p, buf, n) == 0)
            break;
    if (n == 0)
        return -1;
    buf[n] = '\0';
    return memchr(buf, '\0', n) ? 0 : -1;    /* must terminate inside what we read */
}

/* Build a juce::String that actually says what a UTF-8 literal says.
 *
 * FN_STR_CTOR is juce::String(const char*), which is CharPointer_ASCII on this
 * build: one BYTE becomes one codepoint, so "•" arrives as the three Latin-1
 * characters its UTF-8 encoding is made of. Everything above U+007F is wrong
 * through that door, and there is no String::fromUTF8 named in the spec.
 *
 * The way round it is that a juce::String STORES UTF-8. Construct from as many
 * ASCII characters as the literal has BYTES and the buffer is the right size
 * with the right byte count; overwrite those bytes with the literal's own and
 * the same buffer now decodes as the codepoints that were meant. Nothing about
 * the allocation moves, and juce::String carries no cached length to invalidate
 * -- it walks the buffer when asked.
 *
 * The buffer is checked before it is written: a String is one pointer to its
 * text, and this refuses unless what is there is the placeholder it just asked
 * for, at exactly the length it asked for. A firmware that stored strings some
 * other way therefore keeps the placeholder rather than getting a stray write.
 *
 * Pure ASCII takes the plain constructor and none of this. */
void juce_string_utf8(void *str, const char *text)
{
    char probe[JUCE_UTF8_MAX + 1];
    uintptr_t p = 0;
    size_t n = 0, i;
    int wide = 0;

    while (text[n]) {
        if ((unsigned char)text[n] >= 0x80) wide = 1;
        n++;
    }
    if (!wide || n == 0 || n > JUCE_UTF8_MAX) {
        ep_call(void(void *, const char *))::at(FN_STR_CTOR, str, text);
        return;
    }

    for (i = 0; i < n; i++)
        probe[i] = JUCE_UTF8_FILL;
    probe[n] = '\0';
    ep_call(void(void *, const char *))::at(FN_STR_CTOR, str, probe);

    if (mod_safe_read((uintptr_t)str, &p, sizeof(p)) != 0 || !p ||
        mod_safe_read(p, probe, n + 1) != 0 || probe[n] != '\0')
        return;
    for (i = 0; i < n; i++)
        if (probe[i] != JUCE_UTF8_FILL)
            return;                       /* not the buffer we think it is */
    if (mod_safe_write(p, text, n) != 0)
        MDBG("juce: could not place a UTF-8 literal -> it reads as %c%c...\n",
             JUCE_UTF8_FILL, JUCE_UTF8_FILL);
}

void juce_strarray_set(void *sa, const char *const *lines, int n)
{
    int i;

    ep_call(void(void *))::at(FN_STRARR_CTOR, sa);
    for (i = 0; i < n; i++) {
        juce::String s(lines[i]);

        /* add() moves the string in; the empty husk still needs destroying. */
        ep_call(void(void *, void *))::at(FN_STRARR_ADD, sa, s.addr());
    }
}

/* ================================================================== */
/* juce::Component tree                                               */
/* ================================================================== */

int juce_comp_nchild(uintptr_t comp)
{
    int32_t n = 0;

    if (!comp || mod_safe_read(comp + JUCE_NCHILD_OFF, &n, sizeof(n)) != 0)
        return 0;
    /* A plausible ceiling rather than a real one: this walks a live tree from a
     * pointer we inferred, so a field that is not numUsed shows up as a count in
     * the millions and must not become a million reads. */
    return (n < 0 || n > 256) ? 0 : (int)n;
}

uintptr_t juce_comp_child(uintptr_t comp, int i)
{
    uintptr_t kids = 0, c = 0;

    if (!comp || mod_safe_read(comp + JUCE_CHILDREN_OFF, &kids, sizeof(kids)) != 0 || !kids)
        return 0;
    if (mod_safe_read(kids + (uintptr_t)i * sizeof(uintptr_t), &c, sizeof(c)) != 0)
        return 0;
    return c;
}

uintptr_t juce_comp_parent(uintptr_t comp)
{
    uintptr_t p = 0;

    if (!comp || mod_safe_read(comp + JUCE_PARENT_OFF, &p, sizeof(p)) != 0)
        return 0;
    return p;
}

/* The top of the chain. Bounded: a cycle in a tree we are reading out of another
 * process's memory is a hang, not a wrong answer, so it is capped instead. */
uintptr_t juce_comp_root(uintptr_t comp)
{
    int hops;

    for (hops = 0; hops < 32; hops++) {
        uintptr_t p = juce_comp_parent(comp);

        if (!p) break;
        comp = p;
    }
    return comp;
}

int juce_comp_bounds(uintptr_t comp, int32_t out[4])
{
    if (!comp) return -1;
    return mod_safe_read(comp + JUCE_BOUNDS_OFF, out, 4 * sizeof(int32_t));
}

int juce_comp_visible(uintptr_t comp)
{
    uint8_t f = 0;

    return comp && mod_safe_read(comp + JUCE_FLAGS_OFF, &f, 1) == 0 &&
           (f & JUCE_FLAG_VISIBLE);
}

/* The typeinfo word sits one slot before every vtable's address point. */
uintptr_t juce_class_of(uintptr_t vt)
{
    uintptr_t ti = 0;

    if (!vt || mod_safe_read(vt - sizeof(uintptr_t), &ti, sizeof(ti)) != 0)
        return 0;
    return ti;
}

uintptr_t juce_comp_class(uintptr_t comp)
{
    uintptr_t vt = 0;

    if (!comp || mod_safe_read(comp, &vt, sizeof(vt)) != 0)
        return 0;
    return juce_class_of(vt);
}

/* The name is a plain char* at typeinfo+8, which is also where the RTTI walker
 * in resolve.c reads it. Copied a byte at a time through mod_safe_read because
 * the length is not known ahead of the NUL and the pointer came out of another
 * process's live object. */
const char *juce_comp_class_name(uintptr_t comp, char *buf, size_t cap)
{
    uintptr_t ti = juce_comp_class(comp), name = 0;
    size_t i;

    if (!cap) return "";
    buf[0] = '?';
    buf[1] = '\0';
    if (!ti || mod_safe_read(ti + sizeof(uintptr_t), &name, sizeof(name)) != 0 ||
        !name)
        return buf;
    for (i = 0; i + 1 < cap; i++) {
        char c;

        if (mod_safe_read(name + i, &c, 1) != 0)
            break;
        buf[i] = c;
        if (!c)
            return buf;
    }
    buf[i] = '\0';
    return i ? buf : "?";
}

uintptr_t juce_comp_child_of_class(uintptr_t parent, uintptr_t ti)
{
    int n = juce_comp_nchild(parent), i;

    if (!ti) return 0;
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(parent, i);

        if (c && juce_comp_class(c) == ti) return c;
    }
    return 0;
}

/* Depth-first, and bounded for the same reason juce_comp_root() is: this walks
 * another process's live tree, so a bad pointer must cost a wrong answer rather
 * than a hang. */
static uintptr_t juce_find_class_at(uintptr_t comp, uintptr_t ti, int depth)
{
    int n, i;

    if (!comp || depth > 12) return 0;
    if (juce_comp_class(comp) == ti) return comp;
    n = juce_comp_nchild(comp);
    for (i = 0; i < n; i++) {
        uintptr_t hit = juce_find_class_at(juce_comp_child(comp, i), ti, depth + 1);

        if (hit) return hit;
    }
    return 0;
}

uintptr_t juce_comp_find_class(uintptr_t root, uintptr_t ti)
{
    return ti ? juce_find_class_at(root, ti, 0) : 0;
}

/* ================================================================== */
/* juce::Label                                                        */
/* ================================================================== */

void juce_comp_colour(uintptr_t comp, int id, uint32_t argb)
{
    uint32_t c = argb;

    if (comp)
        ep_call(void(void *, int, const void *))
            ::at(FN_COMP_SETCOLOUR, (void *)comp, id, &c);
}


uintptr_t juce_label(uintptr_t parent, const char *text, float font_h,
                     uint32_t bg, uint32_t fg, uintptr_t vptr,
                     int x, int y, int w, int h)
{
    int32_t justify = JUCE_JUSTIFY_CENTRED;
    uintptr_t p;

    p = (uintptr_t)calloc(1, LABEL_ALLOC_SIZE);
    if (!p) return 0;

    {
        juce::String name;
        juce::String body(text, juce::from_utf8);

        ep_call(void(void *, void *, void *))
            ::at(FN_LABEL_CTOR, (void *)p, name.addr(), body.addr());
    }

    if (vptr) *(uintptr_t *)p = vptr;

    {
        juce::Font font(font_h);

        ep_call(void(void *, void *))::at(FN_LABEL_SETFONT, (void *)p, font.addr());
    }
    ep_call(void(void *, void *))::at(FN_LABEL_JUSTIFY, (void *)p, &justify);

    juce_comp_colour(p, LBL_COL_BG,   bg);
    juce_comp_colour(p, LBL_COL_TEXT, fg);

    ep_call(void(void *, int, int, int, int))::at(FN_SET_BOUNDS, (void *)p, x, y, w, h);
    ep_call(void(void *, void *, int))::at(FN_ADD_VISIBLE, (void *)parent, (void *)p, -1);
    return p;
}

void juce_label_justify(uintptr_t label, int justification)
{
    int32_t j = justification;

    if (label && FN_LABEL_JUSTIFY)
        ep_call(void(void *, void *))::at(FN_LABEL_JUSTIFY, (void *)label, &j);
}

void juce_label_text(uintptr_t label, const char *text)
{
    if (!label) return;

    juce::String s(text);
    juce::Var v(s);

    ep_call(void(void *, void *))
        ::at(FN_VALUE_SETVALUE, (void *)(label + LABEL_TEXTVALUE_OFF), v.addr());
}

/* The two words before the address point are offset-to-top and typeinfo, and a
 * clone has to carry them: juce reaches the typeinfo through them. */
#define VT_CLONE_HEAD 2

uintptr_t juce_label_vt_clone(uintptr_t *out, const struct juce_vt_override *ov, int n)
{
    int i;

    if (mod_safe_read(LABEL_VTABLE - VT_CLONE_HEAD * sizeof(uintptr_t), out,
                      VT_CLONE_WORDS * sizeof(uintptr_t)) != 0) {
        MDBG("juce: label vtable %#lx unreadable\n", (unsigned long)LABEL_VTABLE);
        return 0;
    }
    /* The post-condition that says we cloned the class we meant to and that the
     * slot numbering did not move: Label's paint is still in Label's paint slot. */
    if (out[VT_CLONE_HEAD + JUCE_VT_PAINT / sizeof(uintptr_t)] != LABEL_FN_PAINT) {
        MDBG("juce: label paint slot holds %#lx, expected %#lx -> not cloning\n",
             (unsigned long)out[VT_CLONE_HEAD + JUCE_VT_PAINT / sizeof(uintptr_t)],
             (unsigned long)LABEL_FN_PAINT);
        return 0;
    }
    for (i = 0; i < n; i++) {
        uintptr_t *slot = &out[VT_CLONE_HEAD + ov[i].slot / sizeof(uintptr_t)];

        if (ov[i].saved) *ov[i].saved = *slot;
        *slot = (uintptr_t)ov[i].fn;
    }
    return (uintptr_t)&out[VT_CLONE_HEAD];
}
