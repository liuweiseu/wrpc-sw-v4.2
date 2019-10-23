#ifndef __PPSI_PP_TIME_H__
#define __PPSI_PP_TIME_H__

/* Everything internally uses this time format, *signed* */
struct pp_time {
	int64_t		secs;
	int64_t		scaled_nsecs;
};

/* The "correct" bit is hidden in the hight bits */
static inline int is_incorrect(const struct pp_time *t)
{
	return (((t->secs >> 56) & 0xc0) == 0x80);
}
static inline void mark_incorrect(struct pp_time *t)
{
	t->secs |= (0x8fLL << 56);
}
static inline void clear_time(struct pp_time *t)
{
	memset(t, 0, sizeof(*t));
}

#endif /* __PPSI_PP_TIME_H__ */
