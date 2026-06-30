#include <glib.h>

/* Evaluate `expr` as an arithmetic expression. On success writes the formatted
 * result into `out` (NUL-terminated, at most `out_len` bytes) and returns TRUE.
 * Returns FALSE when `expr` is not a self-contained calculation: no operator,
 * an unknown token (e.g. a stray letter), an incomplete expression, or a
 * non-finite result (e.g. 1/0). */
gboolean calc_eval(const char* expr, char* out, gsize out_len);
