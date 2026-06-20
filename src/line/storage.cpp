// storage.cpp -- C++ line storage for the mg.line bridge (task C2b-2).
//
// Under ENABLE_CPP_UPGRADES this *replaces* line.c's malloc/realloc/free line
// storage: `struct line` is owned here with a std::vector<char> (RAII) instead
// of a hand-managed char buffer. The C core sees only an opaque `struct line *`
// and reaches it through these extern "C" accessors (declared in def.h).
//
// Faithful mapping to the legacy fields:
//   buf.size()  <-> l_size   (allocated capacity)
//   used        <-> l_used   (logical length)
//   buf.data()  <-> l_text   (contiguous; NULL when never allocated)
// Allocation failures are caught and reported as NULL / FALSE so no C++
// exception ever crosses back into the C core.

#include <new>
#include <vector>

// The private layout, owned here. Links stay pointer-based (unchanged list
// model); the text is a growable byte vector.
struct line {
    struct line *l_fp = nullptr;
    struct line *l_bp = nullptr;
    int used = 0;                 // logical length (l_used)
    std::vector<char> buf;        // size() is the capacity (l_size)
};

// chrdef.h's CHARMASK, replicated to avoid pulling the C header into C++.
static inline int charmask(char c) { return static_cast<unsigned char>(c); }

extern "C" {

struct line *lalloc(int used)
{
    try {
        line *lp = new line();
        lp->used = used;
        if (used > 0)
            lp->buf.resize(static_cast<std::size_t>(used));
        return lp;
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

// Grow the byte buffer to at least newsize (never shrinks, like the C version).
int lrealloc(struct line *lp, int newsize)
{
    if (static_cast<int>(lp->buf.size()) < newsize) {
        try {
            lp->buf.resize(static_cast<std::size_t>(newsize));
        } catch (const std::bad_alloc &) {
            return 0; // FALSE
        }
    }
    return 1; // TRUE
}

void lfreestore(struct line *lp) { delete lp; }

struct line *lforw(const struct line *lp) { return lp->l_fp; }
struct line *lback(const struct line *lp) { return lp->l_bp; }
int   lgetc(const struct line *lp, int n) { return charmask(lp->buf[n]); }
void  lputc(struct line *lp, int n, int c) { lp->buf[n] = static_cast<char>(c); }
int   llength(const struct line *lp) { return lp->used; }

// NULL when the line never got a buffer, matching the C lalloc(0) -> l_text NULL
// semantics that callers (lreplace, tags.c) test against.
char *ltext(const struct line *lp)
{
    return lp->buf.empty() ? nullptr : const_cast<char *>(lp->buf.data());
}

int   lsize(const struct line *lp) { return static_cast<int>(lp->buf.size()); }
void  lsetlen(struct line *lp, int n) { lp->used = n; }
void  lsetforw(struct line *lp, struct line *lq) { lp->l_fp = lq; }
void  lsetback(struct line *lp, struct line *lq) { lp->l_bp = lq; }

} // extern "C"
