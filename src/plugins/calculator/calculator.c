#include <math.h>
#include <string.h>

#include "calculator.h"
#include "tinyexpr.h"

gboolean calc_eval(const char* expr, char* out, gsize out_len)
{
    /* Only treat the input as a calculation if it actually combines or groups
     * values -- a bare number, an IP (1.2.3.4), or a plain word is a search,
     * not a sum. Requiring one of these characters is what keeps the result
     * label from popping up on ordinary queries. */
    if (strpbrk(expr, "+-*/^%(") == NULL)
        return FALSE;

    int err = 0;
    double v = te_interp(expr, &err);
    if (err != 0 || !isfinite(v)) /* unknown token, partial expression, or 1/0 */
        return FALSE;

    /* %g trims trailing zeros and float noise (4, 2.5, 1.414213562). */
    g_snprintf(out, out_len, "%.10g", v);
    return TRUE;
}
