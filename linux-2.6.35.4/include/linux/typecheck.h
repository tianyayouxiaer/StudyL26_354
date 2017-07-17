#ifndef TYPECHECK_H_INCLUDED
#define TYPECHECK_H_INCLUDED

/*
 * Check at compile time that something is of a particular type.
 * Always evaluates to 1 so you may use it easily in comparisons.
 */
 /*
 第一个是一个类型，比如unsigned long，
 第二个是一个变量，比如a。
 它生成一个unsigned long类型的变量__dummy，
 然后利用typeof生成和a同样类型的变量__dummy2，
 比较__dummy和__dummy2的地址。
 如果它们不是同样类型的指针比较，比如a不是unsigned long，
 这时候编译器会有一个警告，让你注意到这个问题。
 */
#define typecheck(type,x) \
({	type __dummy; \
	typeof(x) __dummy2; \
	(void)(&__dummy == &__dummy2); \
	1; \
})

/*
 * Check at compile time that 'function' is a certain type, or is a pointer
 * to that type (needs to use typedef for the function type.)
 */
#define typecheck_fn(type,function) \
({	typeof(type) __tmp = function; \
	(void)__tmp; \
})

#endif		/* TYPECHECK_H_INCLUDED */
