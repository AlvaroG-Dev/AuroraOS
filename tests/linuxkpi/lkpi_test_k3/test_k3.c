// SPDX-License-Identifier: GPL-2.0-only
/* Pruebas de K3: listas, rbtree, interval_tree, xarray, idr/ida, hashtable. */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/idr.h>
#include <linux/interval_tree.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/llist.h>
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include "lkpi_test.h"

unsigned long lkpi_xa_count(struct xarray *xa);

/* ------------------------------------------------------------- listas */

struct kt_item {
	int key;
	int seq;
	struct list_head node;
	struct hlist_node hnode;
	struct llist_node lnode;
};

static int kt_cmp(void *priv, const struct list_head *a, const struct list_head *b)
{
	return list_entry(a, struct kt_item, node)->key - list_entry(b, struct kt_item, node)->key;
}

static void test_lists(void)
{
	struct kt_item *items = kcalloc(600, sizeof(*items), GFP_KERNEL), *it, *tmp;
	LIST_HEAD(head);
	LIST_HEAD(other);
	u64 seed = 99;
	int i, n, prev_key, prev_seq;
	bool ok = true;

	if (!items) {
		KT_ASSERT(0, "sin memoria");
		return;
	}
	for (i = 0; i < 600; i++) {
		items[i].key = (int)(kt_rand(&seed) % 50);
		items[i].seq = i;
		list_add_tail(&items[i].node, &head);
	}
	KT_EQ(list_count_nodes(&head), 600);
	KT_ASSERT(list_first_entry(&head, struct kt_item, node) == &items[0], "list_first_entry");
	KT_ASSERT(list_last_entry(&head, struct kt_item, node) == &items[599], "list_last_entry");

	list_sort(NULL, &head, kt_cmp);
	prev_key = -1;
	prev_seq = -1;
	n = 0;
	list_for_each_entry(it, &head, node) {
		if (it->key < prev_key || (it->key == prev_key && it->seq < prev_seq))
			ok = false;	/* ordenada y estable */
		prev_key = it->key;
		prev_seq = it->seq;
		n++;
	}
	KT_ASSERT(ok && n == 600, "list_sort ordena y es estable");

	/* Partir, mover y volver a unir. */
	list_cut_position(&other, &head, head.next->next);
	KT_EQ(list_count_nodes(&other), 2);
	KT_EQ(list_count_nodes(&head), 598);
	list_splice_tail_init(&other, &head);
	KT_ASSERT(list_empty(&other) && list_count_nodes(&head) == 600, "list_splice_tail_init");
	list_for_each_entry_safe(it, tmp, &head, node)
		if (it->key % 2)
			list_del(&it->node);
	ok = true;
	list_for_each_entry(it, &head, node)
		if (it->key % 2)
			ok = false;
	KT_ASSERT(ok, "list_del dentro de _safe");
	it = list_first_entry(&head, struct kt_item, node);
	list_move_tail(&it->node, &head);
	KT_ASSERT(list_last_entry(&head, struct kt_item, node) == it, "list_move_tail");
	KT_ASSERT(list_is_last(&it->node, &head), "list_is_last");

	/* hlist */
	{
		HLIST_HEAD(h);
		hlist_add_head(&items[0].hnode, &h);
		hlist_add_head(&items[1].hnode, &h);
		hlist_add_behind(&items[2].hnode, &items[1].hnode);
		n = 0;
		hlist_for_each_entry(it, &h, hnode)
			n = n * 10 + it->seq;
		KT_EQ(n, 120);	/* 1, 2, 0 */
		hlist_del_init(&items[2].hnode);
		KT_EQ(hlist_count_nodes(&h), 2);
	}
	/* llist */
	{
		LLIST_HEAD(l);
		struct llist_node *first;
		llist_add(&items[0].lnode, &l);
		llist_add(&items[1].lnode, &l);
		KT_EQ(llist_add(&items[2].lnode, &l), 0);
		first = llist_reverse_order(llist_del_all(&l));
		n = 0;
		llist_for_each_entry(it, first, lnode)
			n = n * 10 + it->seq + 1;
		KT_EQ(n, 123);
		KT_ASSERT(llist_empty(&l), "llist_del_all");
	}
	kfree(items);
}

/* --------------------------------------------------------------- rbtree */

struct kt_rb {
	struct rb_node rb;
	unsigned long key;
};

/* Altura negra del subarbol, o -1 si se rompe alguna regla. */
static int rb_check(struct rb_node *n, struct rb_node *parent, unsigned long *last, bool *first)
{
	int l, r;
	if (!n)
		return 1;
	if (rb_parent(n) != parent)
		return -1;
	if (rb_is_red(n) && ((n->rb_left && rb_is_red(n->rb_left)) ||
			     (n->rb_right && rb_is_red(n->rb_right))))
		return -1;
	l = rb_check(n->rb_left, n, last, first);
	if (l < 0)
		return -1;
	if (!*first && rb_entry(n, struct kt_rb, rb)->key < *last)
		return -1;
	*first = false;
	*last = rb_entry(n, struct kt_rb, rb)->key;
	r = rb_check(n->rb_right, n, last, first);
	if (r < 0 || l != r)
		return -1;
	return l + (rb_is_black(n) ? 1 : 0);
}

static bool rb_valid(struct rb_root *root)
{
	unsigned long last = 0;
	bool first = true;
	if (root->rb_node && !rb_is_black(root->rb_node))
		return false;
	return rb_check(root->rb_node, NULL, &last, &first) > 0;
}

static bool kt_rb_less(struct rb_node *a, const struct rb_node *b)
{
	return rb_entry(a, struct kt_rb, rb)->key < rb_entry(b, struct kt_rb, rb)->key;
}

static int kt_rb_cmp(const void *key, const struct rb_node *n)
{
	unsigned long k = *(const unsigned long *)key, nk = rb_entry(n, struct kt_rb, rb)->key;
	return k < nk ? -1 : k > nk;
}

static void test_rbtree(void)
{
	const int N = 3000;
	struct kt_rb *nodes = kcalloc(N, sizeof(*nodes), GFP_KERNEL);
	struct rb_root_cached root = RB_ROOT_CACHED;
	struct rb_node *n;
	u64 seed = 4242;
	unsigned long minkey = ULONG_MAX, prev;
	int i, count;
	bool ok = true;

	if (!nodes) {
		KT_ASSERT(0, "sin memoria");
		return;
	}
	for (i = 0; i < N; i++) {
		nodes[i].key = kt_rand(&seed) % 100000;
		minkey = min(minkey, nodes[i].key);
		rb_add_cached(&nodes[i].rb, &root, kt_rb_less);
		if (i % 500 == 0 && !rb_valid(&root.rb_root))
			ok = false;
	}
	KT_ASSERT(ok && rb_valid(&root.rb_root), "invariantes tras insertar");
	KT_EQ(rb_entry(rb_first_cached(&root), struct kt_rb, rb)->key, minkey);
	KT_ASSERT(rb_first_cached(&root) == rb_first(&root.rb_root), "leftmost cacheado");
	count = 0;
	prev = 0;
	ok = true;
	for (n = rb_first(&root.rb_root); n; n = rb_next(n)) {
		if (rb_entry(n, struct kt_rb, rb)->key < prev)
			ok = false;
		prev = rb_entry(n, struct kt_rb, rb)->key;
		count++;
	}
	KT_ASSERT(ok && count == N, "recorrido en orden (%d)", count);
	count = 0;
	for (n = rb_last(&root.rb_root); n; n = rb_prev(n))
		count++;
	KT_EQ(count, N);
	{
		unsigned long key = nodes[1234].key;
		struct rb_node *f = rb_find(&key, &root.rb_root, kt_rb_cmp);
		KT_ASSERT(f && rb_entry(f, struct kt_rb, rb)->key == key, "rb_find");
	}

	/* Borrar dos tercios en orden pseudoaleatorio. */
	ok = true;
	for (i = 0; i < N; i++) {
		if (i % 3 == 0)
			continue;
		rb_erase_cached(&nodes[i].rb, &root);
		RB_CLEAR_NODE(&nodes[i].rb);
		if (i % 400 == 1 && !rb_valid(&root.rb_root))
			ok = false;
	}
	KT_ASSERT(ok && rb_valid(&root.rb_root), "invariantes tras borrar");
	KT_ASSERT(RB_EMPTY_NODE(&nodes[1].rb), "RB_CLEAR_NODE");
	KT_ASSERT(rb_first_cached(&root) == rb_first(&root.rb_root), "leftmost tras borrar");
	count = 0;
	for (n = rb_first(&root.rb_root); n; n = rb_next(n))
		count++;
	KT_EQ(count, (N + 2) / 3);

	/* rb_replace_node y recorrido postorden. */
	{
		struct kt_rb repl;
		repl.key = nodes[0].key;
		rb_replace_node_cached(&nodes[0].rb, &repl.rb, &root);
		KT_ASSERT(rb_valid(&root.rb_root), "rb_replace_node");
		struct kt_rb *pos, *nxt;
		count = 0;
		rbtree_postorder_for_each_entry_safe(pos, nxt, &root.rb_root, rb)
			count++;
		KT_EQ(count, (N + 2) / 3);
	}
	kfree(nodes);
}

/* --------------------------------------------------------- interval_tree */

static void test_interval_tree(void)
{
	const int N = 400;
	struct interval_tree_node *iv = kcalloc(N, sizeof(*iv), GFP_KERNEL);
	struct rb_root_cached root = RB_ROOT_CACHED;
	u64 seed = 777;
	bool ok = true;
	int i, q;

	if (!iv) {
		KT_ASSERT(0, "sin memoria");
		return;
	}
	for (i = 0; i < N; i++) {
		iv[i].start = kt_rand(&seed) % 10000;
		iv[i].last = iv[i].start + kt_rand(&seed) % 300;
		interval_tree_insert(&iv[i], &root);
	}
	for (i = 0; i < N; i += 3)
		interval_tree_remove(&iv[i], &root);
	for (q = 0; q < 200; q++) {
		unsigned long s = kt_rand(&seed) % 10300, l = s + kt_rand(&seed) % 200;
		int expect = 0, got = 0;
		struct interval_tree_node *it;
		for (i = 0; i < N; i++)
			if (i % 3 && iv[i].start <= l && s <= iv[i].last)
				expect++;
		for (it = interval_tree_iter_first(&root, s, l); it;
		     it = interval_tree_iter_next(it, s, l)) {
			if (!(it->start <= l && s <= it->last))
				ok = false;
			got++;
		}
		if (got != expect)
			ok = false;
	}
	KT_ASSERT(ok, "interval_tree coincide con la busqueda exhaustiva");
	kfree(iv);
}

/* --------------------------------------------------------------- xarray */

static void test_xarray(void)
{
	DEFINE_XARRAY(xa);
	struct xarray xa2;
	static const unsigned long idx[] = { 0, 1, 63, 64, 4095, 1UL << 20, 1UL << 40,
					     ULONG_MAX - 1, ULONG_MAX };
	unsigned long index;
	void *entry;
	u32 id;
	int i, n;
	bool ok = true;

	for (i = 0; i < (int)ARRAY_SIZE(idx); i++)
		if (xa_err(xa_store(&xa, idx[i], xa_mk_value(i), GFP_KERNEL)))
			ok = false;
	KT_ASSERT(ok, "xa_store en indices dispersos");
	for (i = 0; i < (int)ARRAY_SIZE(idx); i++)
		if (xa_load(&xa, idx[i]) != xa_mk_value(i))
			ok = false;
	KT_ASSERT(ok, "xa_load");
	KT_ASSERT(xa_load(&xa, 2) == NULL && xa_load(&xa, 1UL << 39) == NULL, "huecos vacios");
	n = 0;
	index = 0;
	xa_for_each(&xa, index, entry) {
		if (entry != xa_mk_value(n) || index != idx[n])
			ok = false;
		n++;
	}
	KT_ASSERT(ok && n == (int)ARRAY_SIZE(idx), "xa_for_each en orden (%d)", n);
	KT_EQ(xa_to_value(xa_erase(&xa, 64)), 3);
	KT_ASSERT(xa_load(&xa, 64) == NULL, "xa_erase");
	KT_EQ(xa_insert(&xa, 63, xa_mk_value(9), GFP_KERNEL), -EBUSY);
	KT_EQ(xa_insert(&xa, 64, xa_mk_value(9), GFP_KERNEL), 0);
	KT_ASSERT(xa_cmpxchg(&xa, 64, xa_mk_value(9), xa_mk_value(10), GFP_KERNEL) ==
		  xa_mk_value(9), "xa_cmpxchg");
	KT_ASSERT(xa_load(&xa, 64) == xa_mk_value(10), "xa_cmpxchg guarda");

	xa_set_mark(&xa, 4095, XA_MARK_1);
	xa_set_mark(&xa, 1UL << 40, XA_MARK_1);
	KT_EQ(xa_get_mark(&xa, 4095, XA_MARK_1), 1);
	KT_EQ(xa_get_mark(&xa, 63, XA_MARK_1), 0);
	n = 0;
	xa_for_each_marked(&xa, index, entry, XA_MARK_1)
		n++;
	KT_EQ(n, 2);
	xa_clear_mark(&xa, 4095, XA_MARK_1);
	n = 0;
	xa_for_each_marked(&xa, index, entry, XA_MARK_1)
		n++;
	KT_EQ(n, 1);
	KT_EQ(xa_marked(&xa, XA_MARK_2), 0);
	for (i = 0; i < (int)ARRAY_SIZE(idx); i++)
		xa_erase(&xa, idx[i]);
	KT_ASSERT(xa_empty(&xa), "xarray vacio tras borrar todo");

	/* Asignacion de ids */
	xa_init_flags(&xa2, XA_FLAGS_ALLOC1);
	ok = true;
	for (i = 0; i < 300; i++)
		if (xa_alloc(&xa2, &id, xa_mk_value(i), xa_limit_32b, GFP_KERNEL) || id != (u32)i + 1)
			ok = false;
	KT_ASSERT(ok, "xa_alloc secuencial desde 1");
	xa_erase(&xa2, 50);
	xa_erase(&xa2, 200);
	KT_EQ(xa_alloc(&xa2, &id, xa_mk_value(0), xa_limit_32b, GFP_KERNEL), 0);
	KT_EQ(id, 50);
	KT_EQ(xa_alloc(&xa2, &id, xa_mk_value(0), XA_LIMIT(195, 210), GFP_KERNEL), 0);
	KT_EQ(id, 200);
	KT_EQ(xa_alloc(&xa2, &id, xa_mk_value(0), XA_LIMIT(1, 3), GFP_KERNEL), -EBUSY);
	KT_EQ(xa_reserve(&xa2, 1000, GFP_KERNEL), 0);
	KT_ASSERT(xa_load(&xa2, 1000) == NULL, "reservado se lee como NULL");
	KT_EQ(xa_insert(&xa2, 1000, xa_mk_value(1), GFP_KERNEL), -EBUSY);
	xa_release(&xa2, 1000);
	KT_EQ(xa_insert(&xa2, 1000, xa_mk_value(1), GFP_KERNEL), 0);
	KT_EQ(lkpi_xa_count(&xa2), 301);
	xa_destroy(&xa2);
	KT_ASSERT(xa_empty(&xa2), "xa_destroy");
}

/* ------------------------------------------------------------------ idr */

static int idr_counter(int id, void *p, void *data)
{
	(*(int *)data)++;
	return 0;
}

static void test_idr(void)
{
	DEFINE_IDR(idr);
	struct idr idr1;
	DEFINE_IDA(ida);
	int i, id, n;
	void *p;
	bool ok = true;

	for (i = 0; i < 500; i++)
		if (idr_alloc(&idr, (void *)(uintptr_t)(0x1000 + i * 8), 0, 0, GFP_KERNEL) != i)
			ok = false;
	KT_ASSERT(ok, "idr_alloc secuencial");
	KT_ASSERT(idr_find(&idr, 77) == (void *)(0x1000 + 77 * 8), "idr_find");
	KT_ASSERT(idr_remove(&idr, 77) == (void *)(0x1000 + 77 * 8), "idr_remove");
	KT_ASSERT(idr_find(&idr, 77) == NULL, "idr_find tras remove");
	KT_EQ(idr_alloc(&idr, (void *)0x2000, 0, 0, GFP_KERNEL), 77);
	KT_EQ(idr_alloc(&idr, (void *)0x2008, 1000, 1001, GFP_KERNEL), 1000);
	KT_EQ(idr_alloc(&idr, (void *)0x2010, 1000, 1001, GFP_KERNEL), -ENOSPC);
	KT_ASSERT(idr_replace(&idr, (void *)0x3000, 1000) == (void *)0x2008, "idr_replace");
	KT_ASSERT(IS_ERR(idr_replace(&idr, (void *)0x3000, 999)), "idr_replace sin id");
	/* NULL reserva el id */
	id = idr_alloc(&idr, NULL, 600, 0, GFP_KERNEL);
	KT_EQ(id, 600);
	KT_ASSERT(idr_find(&idr, 600) == NULL, "id reservado");
	KT_EQ(idr_alloc(&idr, (void *)0x4000, 600, 602, GFP_KERNEL), 601);
	n = 0;
	idr_for_each(&idr, idr_counter, &n);
	KT_EQ(n, 502);	/* 0..499 + 1000 + 601 (el 600 reservado no cuenta) */
	n = 0;
	id = 0;
	idr_for_each_entry(&idr, p, id)
		n++;
	KT_EQ(n, 502);
	id = 499;
	KT_ASSERT(idr_get_next(&idr, &id) && id == 499, "idr_get_next");
	id = 500;
	KT_ASSERT(idr_get_next(&idr, &id) && id == 601, "idr_get_next salta huecos");
	KT_EQ(idr_alloc_cyclic(&idr, (void *)0x5000, 0, 0, GFP_KERNEL), 500);
	KT_EQ(idr_alloc_cyclic(&idr, (void *)0x5008, 0, 0, GFP_KERNEL), 501);
	idr_remove(&idr, 3);
	KT_EQ(idr_alloc_cyclic(&idr, (void *)0x5010, 0, 0, GFP_KERNEL), 502);	/* no vuelve al 3 */
	idr_destroy(&idr);
	KT_ASSERT(idr_is_empty(&idr), "idr_destroy");

	idr_init_base(&idr1, 1);
	KT_EQ(idr_alloc(&idr1, (void *)0x10, 0, 0, GFP_KERNEL), 1);	/* empieza en la base */
	KT_EQ(idr_alloc(&idr1, (void *)0x18, 1, 0, GFP_KERNEL), 2);
	KT_ASSERT(idr_find(&idr1, 2) == (void *)0x18 && idr_find(&idr1, 0) == NULL, "idr base 1");
	idr_destroy(&idr1);

	KT_EQ(ida_alloc(&ida, GFP_KERNEL), 0);
	KT_EQ(ida_alloc(&ida, GFP_KERNEL), 1);
	KT_EQ(ida_alloc_min(&ida, 10, GFP_KERNEL), 10);
	KT_EQ(ida_alloc_range(&ida, 0, 1, GFP_KERNEL), -ENOSPC);
	ida_free(&ida, 0);
	KT_EQ(ida_alloc(&ida, GFP_KERNEL), 0);
	ida_destroy(&ida);
	KT_ASSERT(ida_is_empty(&ida), "ida_destroy");
}

/* ------------------------------------------------------------ hashtable */

static void test_hashtable(void)
{
	DEFINE_HASHTABLE(ht, 6);
	struct kt_item items[100], *it;
	struct hlist_node *tmp;
	int i, bkt, n = 0, found = 0;

	for (i = 0; i < 100; i++) {
		items[i].key = i * 37;
		hash_add(ht, &items[i].hnode, items[i].key);
	}
	hash_for_each_possible(ht, it, hnode, 37 * 42)
		if (it->key == 37 * 42)
			found++;
	KT_EQ(found, 1);
	hash_for_each(ht, bkt, it, hnode)
		n++;
	KT_EQ(n, 100);
	hash_for_each_safe(ht, bkt, tmp, it, hnode)
		hash_del(&it->hnode);
	KT_ASSERT(hash_empty(ht), "hash_empty");
}

static int __init test_k3_init(void)
{
	test_lists();
	test_rbtree();
	test_interval_tree();
	test_xarray();
	test_idr();
	test_hashtable();
	return 0;
}

static void __exit test_k3_exit(void)
{
}

module_init(test_k3_init);
module_exit(test_k3_exit);
MODULE_LICENSE("GPL");
