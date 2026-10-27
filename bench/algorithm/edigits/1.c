#include <errno.h>
#include <gmp.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void sum_terms(mpz_t p, mpz_t q, unsigned long a,
                      unsigned long b) {
    if (b == a + 1) {
        mpz_set_ui(p, 1);
        mpz_set_ui(q, b);
        return;
    }

    unsigned long mid = a + (b - a) / 2;
    mpz_t pl, ql, pr, qr;
    mpz_inits(pl, ql, pr, qr, NULL);

    sum_terms(pl, ql, a, mid);
    sum_terms(pr, qr, mid, b);

    mpz_mul(p, pl, qr);
    mpz_add(p, p, pr);
    mpz_mul(q, ql, qr);

    mpz_clears(pl, ql, pr, qr, NULL);
}

static int enough_terms(unsigned long n, unsigned long k) {
    if (!k) return 0;
    double x = (double)k;
    double ln_fact = x * log(x) - x +
        0.5 * log(2.0 * 3.14159265358979323846 * x);
    return ln_fact / log(10.0) >= (double)n + 50.0;
}

static unsigned long term_count(unsigned long n) {
    unsigned long a = 0, b = 1;
    while (!enough_terms(n, b)) {
        if (b > ULONG_MAX / 2) {
            fprintf(stderr, "input too large\n");
            exit(EXIT_FAILURE);
        }
        a = b;
        b *= 2;
    }
    while (b - a > 1) {
        unsigned long m = a + (b - a) / 2;
        if (enough_terms(n, m)) b = m;
        else a = m;
    }
    return b;
}

int main(int argc, char **argv) {
    unsigned long n = 27;
    if (argc > 1) {
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (errno || end == argv[1] || *end ||
            !parsed || argv[1][0] == '-') {
            fprintf(stderr, "usage: %s [positive-digit-count]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        n = parsed;
    }
    if (n == ULONG_MAX) return EXIT_FAILURE;

    unsigned long k = term_count(n);

    mpz_t p, q, scale, value;
    mpz_inits(p, q, scale, value, NULL);

    sum_terms(p, q, 0, k - 1);
    mpz_add(p, p, q);
    mpz_ui_pow_ui(scale, 10, n - 1);
    mpz_mul(value, p, scale);
    mpz_tdiv_q(value, value, q);

    size_t cap = mpz_sizeinbase(value, 10) + 2;
    char *s = malloc(cap);
    if (!s) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    mpz_get_str(s, 10, value);

    for (unsigned long i = 0; i < n; i += 10) {
        unsigned long count = (n - i < 10) ? n - i : 10;
        fwrite(s + i, 1, (size_t)count, stdout);
        for (unsigned long j = count; j < 10; ++j)
            putchar(' ');
        printf("\t:%lu\n", i + count);
    }

    free(s);
    mpz_clears(p, q, scale, value, NULL);
    return 0;
}
