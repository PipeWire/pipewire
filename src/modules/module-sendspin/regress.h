
struct spa_regress {
	double meanX;
	double meanY;
	double varX;
	double covXY;
	uint32_t n;
	uint32_t m;
	double a;
};

static inline void spa_regress_init(struct spa_regress *r, uint32_t m)
{
	memset(r, 0, sizeof(*r));
	r->m = m;
	r->a = 1.0/m;
}
static inline void spa_regress_update(struct spa_regress *r, double x, double y)
{
	double a, dx, dy;

	if (r->n == 0) {
		r->meanX = x;
		r->meanY = y;
		r->n++;
		a = 1.0;
	} else if (r->n < r->m) {
		a = 1.0/r->n;
		r->n++;
	} else {
		a = r->a;
	}
	dx = x - r->meanX;
	dy = y - r->meanY;

	r->varX += ((1.0 - a) * dx * dx - r->varX) * a;
	r->covXY += ((1.0 - a) * dx * dy - r->covXY) * a;
	r->meanX += dx * a;
	r->meanY += dy * a;
}
static inline void spa_regress_get(struct spa_regress *r, double *a, double *b)
{
	*a = r->covXY/r->varX;
	*b = r->meanY - *a * r->meanX;
}
static inline double spa_regress_calc_y(struct spa_regress *r, double x)
{
	double a, b;
	spa_regress_get(r, &a, &b);
	return x * a + b;
}
static inline double spa_regress_calc_x(struct spa_regress *r, double y)
{
	double a, b;
	spa_regress_get(r, &a, &b);
	return (y - b) / a;
}

