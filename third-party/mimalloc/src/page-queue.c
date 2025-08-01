/*----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  Definition of page queues for each block size
----------------------------------------------------------- */

#ifndef MI_IN_PAGE_C
#error "this file should be included from 'page.c'"
// include to help an IDE
#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/atomic.h"
#endif

/* -----------------------------------------------------------
  Minimal alignment in machine words (i.e. `sizeof(void*)`)
----------------------------------------------------------- */

#if (MI_MAX_ALIGN_SIZE > 4*MI_INTPTR_SIZE)
  #error "define alignment for more than 4x word size for this platform"
#elif (MI_MAX_ALIGN_SIZE > 2*MI_INTPTR_SIZE)
  #define MI_ALIGN4W   // 4 machine words minimal alignment
#elif (MI_MAX_ALIGN_SIZE > MI_INTPTR_SIZE)
  #define MI_ALIGN2W   // 2 machine words minimal alignment
#else
  // ok, default alignment is 1 word
#endif


/* -----------------------------------------------------------
  Queue query
----------------------------------------------------------- */


static inline bool mi_page_queue_is_huge(const mi_page_queue_t* pq) {
  return (pq->block_size == (MI_LARGE_MAX_OBJ_SIZE+sizeof(uintptr_t)));
}

static inline bool mi_page_queue_is_full(const mi_page_queue_t* pq) {
  return (pq->block_size == (MI_LARGE_MAX_OBJ_SIZE+(2*sizeof(uintptr_t))));
}

static inline bool mi_page_queue_is_special(const mi_page_queue_t* pq) {
  return (pq->block_size > MI_LARGE_MAX_OBJ_SIZE);
}

static inline size_t mi_page_queue_count(const mi_page_queue_t* pq) {
  return pq->count;
}

/* -----------------------------------------------------------
  Bins
----------------------------------------------------------- */

// Return the bin for a given field size.
// Returns MI_BIN_HUGE if the size is too large.
// We use `wsize` for the size in "machine word sizes",
// i.e. byte size == `wsize*sizeof(void*)`.
static mi_decl_noinline size_t mi_bin(size_t size) {
  size_t wsize = _mi_wsize_from_size(size);
#if defined(MI_ALIGN4W)
  if mi_likely(wsize <= 4) {
    return (wsize <= 1 ? 1 : (wsize+1)&~1); // round to double word sizes
  }
#elif defined(MI_ALIGN2W)
  if mi_likely(wsize <= 8) {
    return (wsize <= 1 ? 1 : (wsize+1)&~1); // round to double word sizes
  }
#else
  if mi_likely(wsize <= 8) {
    return (wsize == 0 ? 1 : wsize);
  }
#endif
  else if mi_unlikely(wsize > MI_LARGE_MAX_OBJ_WSIZE) {
    return MI_BIN_HUGE;
  }
  else {
    #if defined(MI_ALIGN4W)
    if (wsize <= 16) { wsize = (wsize+3)&~3; } // round to 4x word sizes
    #endif
    wsize--;
    // find the highest bit
    const size_t b = (MI_SIZE_BITS - 1 - mi_clz(wsize));  // note: wsize != 0
    // and use the top 3 bits to determine the bin (~12.5% worst internal fragmentation).
    // - adjust with 3 because we use do not round the first 8 sizes
    //   which each get an exact bin
    const size_t bin = ((b << 2) + ((wsize >> (b - 2)) & 0x03)) - 3;
    mi_assert_internal(bin > 0 && bin < MI_BIN_HUGE);
    return bin;
  }
}



/* -----------------------------------------------------------
  Queue of pages with free blocks
----------------------------------------------------------- */

size_t _mi_bin(size_t size) {
  return mi_bin(size);
}

size_t _mi_bin_size(size_t bin) {
  return _mi_heap_empty.pages[bin].block_size;
}

// Good size for allocation
mi_decl_nodiscard mi_decl_export size_t mi_good_size(size_t size) mi_attr_noexcept {
  if (size <= MI_LARGE_MAX_OBJ_SIZE) {
    return _mi_bin_size(mi_bin(size + MI_PADDING_SIZE));
  }
  else {
    return _mi_align_up(size + MI_PADDING_SIZE,_mi_os_page_size());
  }
}

#if (MI_DEBUG>1)
static bool mi_page_queue_contains(mi_page_queue_t* queue, const mi_page_t* page) {
  mi_assert_internal(page != NULL);
  mi_page_t* list = queue->first;
  while (list != NULL) {
    mi_assert_internal(list->next == NULL || list->next->prev == list);
    mi_assert_internal(list->prev == NULL || list->prev->next == list);
    if (list == page) break;
    list = list->next;
  }
  return (list == page);
}

#endif

#if (MI_DEBUG>1)
static bool mi_heap_contains_queue(const mi_heap_t* heap, const mi_page_queue_t* pq) {
  return (pq >= &heap->pages[0] && pq <= &heap->pages[MI_BIN_FULL]);
}
#endif

bool _mi_page_queue_is_valid(mi_heap_t* heap, const mi_page_queue_t* pq) {
  MI_UNUSED_RELEASE(heap);
  if (pq==NULL) return false;
  size_t count = 0; MI_UNUSED_RELEASE(count);
  mi_page_t* prev = NULL; MI_UNUSED_RELEASE(prev);
  for (mi_page_t* page = pq->first; page != NULL; page = page->next) {
    mi_assert_internal(page->prev == prev);
    if (mi_page_is_in_full(page)) {
      mi_assert_internal(_mi_wsize_from_size(pq->block_size) == MI_LARGE_MAX_OBJ_WSIZE + 2); 
    }
    else if (mi_page_is_huge(page)) {
      mi_assert_internal(_mi_wsize_from_size(pq->block_size) == MI_LARGE_MAX_OBJ_WSIZE + 1); 
    }
    else {
      mi_assert_internal(mi_page_block_size(page) == pq->block_size);
    }
    mi_assert_internal(page->heap == heap);
    if (page->next == NULL) {
      mi_assert_internal(pq->last == page);
    }
    count++;
    prev = page;
  }
  mi_assert_internal(pq->count == count);
  return true;
}

size_t _mi_page_bin(const mi_page_t* page) {
  const size_t bin = (mi_page_is_in_full(page) ? MI_BIN_FULL : (mi_page_is_huge(page) ? MI_BIN_HUGE : mi_bin(mi_page_block_size(page))));
  mi_assert_internal(bin <= MI_BIN_FULL);
  return bin;
}

static mi_page_queue_t* mi_heap_page_queue_of(mi_heap_t* heap, const mi_page_t* page) {
  mi_assert_internal(heap!=NULL);
  size_t bin = (mi_page_is_in_full(page) ? MI_BIN_FULL : (mi_page_is_huge(page) ? MI_BIN_HUGE : mi_bin(mi_page_block_size(page))));
  mi_assert_internal(bin <= MI_BIN_FULL);
  mi_page_queue_t* pq = &heap->pages[bin];
  mi_assert_internal((mi_page_block_size(page) == pq->block_size) ||
                       (mi_page_is_huge(page) && mi_page_queue_is_huge(pq)) ||
                         (mi_page_is_in_full(page) && mi_page_queue_is_full(pq)));
  return pq;
}

static mi_page_queue_t* mi_page_queue_of(const mi_page_t* page) {
  mi_heap_t* heap = mi_page_heap(page);
  mi_page_queue_t* pq = mi_heap_page_queue_of(heap, page);
  mi_assert_expensive(mi_page_queue_contains(pq, page));
  return pq;
}

// The current small page array is for efficiency and for each
// small size (up to 256) it points directly to the page for that
// size without having to compute the bin. This means when the
// current free page queue is updated for a small bin, we need to update a
// range of entries in `_mi_page_small_free`.
static inline void mi_heap_queue_first_update(mi_heap_t* heap, const mi_page_queue_t* pq) {
  mi_assert_internal(mi_heap_contains_queue(heap,pq));
  size_t size = pq->block_size;
  if (size > MI_SMALL_SIZE_MAX) return;

  mi_page_t* page = pq->first;
  if (pq->first == NULL) page = (mi_page_t*)&_mi_page_empty;

  // find index in the right direct page array
  size_t start;
  size_t idx = _mi_wsize_from_size(size);
  mi_page_t** pages_free = heap->pages_free_direct;

  if (pages_free[idx] == page) return;  // already set

  // find start slot
  if (idx<=1) {
    start = 0;
  }
  else {
    // find previous size; due to minimal alignment upto 3 previous bins may need to be skipped
    size_t bin = mi_bin(size);
    const mi_page_queue_t* prev = pq - 1;
    while( bin == mi_bin(prev->block_size) && prev > &heap->pages[0]) {
      prev--;
    }
    start = 1 + _mi_wsize_from_size(prev->block_size);
    if (start > idx) start = idx;
  }

  // set size range to the right page
  mi_assert(start <= idx);
  for (size_t sz = start; sz <= idx; sz++) {
    pages_free[sz] = page;
  }
}

/*
static bool mi_page_queue_is_empty(mi_page_queue_t* queue) {
  return (queue->first == NULL);
}
*/

static void mi_page_queue_remove(mi_page_queue_t* queue, mi_page_t* page) {
  mi_assert_internal(page != NULL);
  mi_assert_expensive(mi_page_queue_contains(queue, page));
  mi_assert_internal(queue->count >= 1);
  mi_assert_internal(mi_page_block_size(page) == queue->block_size ||
                      (mi_page_is_huge(page) && mi_page_queue_is_huge(queue)) ||
                        (mi_page_is_in_full(page) && mi_page_queue_is_full(queue)));
  mi_heap_t* heap = mi_page_heap(page);
  if (page->prev != NULL) page->prev->next = page->next;
  if (page->next != NULL) page->next->prev = page->prev;
  if (page == queue->last)  queue->last = page->prev;
  if (page == queue->first) {
    queue->first = page->next;
    // update first
    mi_assert_internal(mi_heap_contains_queue(heap, queue));
    mi_heap_queue_first_update(heap,queue);
  }
  heap->page_count--;
  queue->count--;
  page->next = NULL;
  page->prev = NULL;
  mi_page_set_in_full(page,false);
}


static void mi_page_queue_push(mi_heap_t* heap, mi_page_queue_t* queue, mi_page_t* page) {
  mi_assert_internal(mi_page_heap(page) == heap);
  mi_assert_internal(!mi_page_queue_contains(queue, page));
  #if MI_HUGE_PAGE_ABANDON
  mi_assert_internal(_mi_page_segment(page)->page_kind != MI_PAGE_HUGE);
  #endif
  mi_assert_internal(mi_page_block_size(page) == queue->block_size ||
                      (mi_page_is_huge(page) && mi_page_queue_is_huge(queue)) ||
                        (mi_page_is_in_full(page) && mi_page_queue_is_full(queue)));

  mi_page_set_in_full(page, mi_page_queue_is_full(queue));

  page->next = queue->first;
  page->prev = NULL;
  if (queue->first != NULL) {
    mi_assert_internal(queue->first->prev == NULL);
    queue->first->prev = page;
    queue->first = page;
  }
  else {
    queue->first = queue->last = page;
  }
  queue->count++;

  // update direct
  mi_heap_queue_first_update(heap, queue);
  heap->page_count++;
}

static void mi_page_queue_push_at_end(mi_heap_t* heap, mi_page_queue_t* queue, mi_page_t* page) {
  mi_assert_internal(mi_page_heap(page) == heap);
  mi_assert_internal(!mi_page_queue_contains(queue, page));

  mi_assert_internal(mi_page_block_size(page) == queue->block_size ||
                      (mi_page_is_huge(page) && mi_page_queue_is_huge(queue)) ||
                       (mi_page_is_in_full(page) && mi_page_queue_is_full(queue)));

  mi_page_set_in_full(page, mi_page_queue_is_full(queue));

  page->prev = queue->last;
  page->next = NULL;
  if (queue->last != NULL) {
    mi_assert_internal(queue->last->next == NULL);
    queue->last->next = page;
    queue->last = page;
  }
  else {
    queue->first = queue->last = page;
  }
  queue->count++;

  // update direct
  if (queue->first == page) {
    mi_heap_queue_first_update(heap, queue);
  }
  heap->page_count++;
}

static void mi_page_queue_move_to_front(mi_heap_t* heap, mi_page_queue_t* queue, mi_page_t* page) {
  mi_assert_internal(mi_page_heap(page) == heap);
  mi_assert_internal(mi_page_queue_contains(queue, page));
  if (queue->first == page) return;
  mi_page_queue_remove(queue, page);
  mi_page_queue_push(heap, queue, page);
  mi_assert_internal(queue->first == page);
}

static void mi_page_queue_enqueue_from_ex(mi_page_queue_t* to, mi_page_queue_t* from, bool enqueue_at_end, mi_page_t* page) {
  mi_assert_internal(page != NULL);
  mi_assert_internal(from->count >= 1);
  mi_assert_expensive(mi_page_queue_contains(from, page));
  mi_assert_expensive(!mi_page_queue_contains(to, page));
  const size_t bsize = mi_page_block_size(page);
  MI_UNUSED(bsize);
  mi_assert_internal((bsize == to->block_size && bsize == from->block_size) ||
                     (bsize == to->block_size && mi_page_queue_is_full(from)) ||
                     (bsize == from->block_size && mi_page_queue_is_full(to)) ||
                     (mi_page_is_huge(page) && mi_page_queue_is_huge(to)) ||
                     (mi_page_is_huge(page) && mi_page_queue_is_full(to)));

  mi_heap_t* heap = mi_page_heap(page);

  // delete from `from`
  if (page->prev != NULL) page->prev->next = page->next;
  if (page->next != NULL) page->next->prev = page->prev;
  if (page == from->last)  from->last = page->prev;
  if (page == from->first) {
    from->first = page->next;
    // update first
    mi_assert_internal(mi_heap_contains_queue(heap, from));
    mi_heap_queue_first_update(heap, from);
  }
  from->count--;

  // insert into `to`
  to->count++;
  if (enqueue_at_end) {
    // enqueue at the end
    page->prev = to->last;
    page->next = NULL;
    if (to->last != NULL) {
      mi_assert_internal(heap == mi_page_heap(to->last));
      to->last->next = page;
      to->last = page;
    }
    else {
      to->first = page;
      to->last = page;
      mi_heap_queue_first_update(heap, to);
    }
  }
  else {
    if (to->first != NULL) {
      // enqueue at 2nd place
      mi_assert_internal(heap == mi_page_heap(to->first));
      mi_page_t* next = to->first->next;
      page->prev = to->first;
      page->next = next;
      to->first->next = page;
      if (next != NULL) {
        next->prev = page;
      }
      else {
        to->last = page;
      }
    }
    else {
      // enqueue at the head (singleton list)
      page->prev = NULL;
      page->next = NULL;
      to->first = page;
      to->last = page;
      mi_heap_queue_first_update(heap, to);
    }
  }

  mi_page_set_in_full(page, mi_page_queue_is_full(to));
}

static void mi_page_queue_enqueue_from(mi_page_queue_t* to, mi_page_queue_t* from, mi_page_t* page) {
  mi_page_queue_enqueue_from_ex(to, from, true /* enqueue at the end */, page);
}

static void mi_page_queue_enqueue_from_full(mi_page_queue_t* to, mi_page_queue_t* from, mi_page_t* page) {
  // note: we could insert at the front to increase reuse, but it slows down certain benchmarks (like `alloc-test`)
  mi_page_queue_enqueue_from_ex(to, from, true /* enqueue at the end of the `to` queue? */, page);
}

// Only called from `mi_heap_absorb`.
size_t _mi_page_queue_append(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_queue_t* append) {
  mi_assert_internal(mi_heap_contains_queue(heap,pq));
  mi_assert_internal(pq->block_size == append->block_size);
  
  if (append->first==NULL) return 0;
  
  // set append pages to new heap and count
  size_t count = 0;
  for (mi_page_t* page = append->first; page != NULL; page = page->next) {
    mi_page_set_heap(page, heap);
    count++;
  }
  mi_assert_internal(count == append->count);

  if (pq->last==NULL) {
    // take over afresh
    mi_assert_internal(pq->first==NULL);
    pq->first = append->first;
    pq->last = append->last;
    mi_heap_queue_first_update(heap, pq);
  }
  else {
    // append to end
    mi_assert_internal(pq->last!=NULL);
    mi_assert_internal(append->first!=NULL);
    pq->last->next = append->first;
    append->first->prev = pq->last;
    pq->last = append->last;
  }
  pq->count += append->count;

  return count;
}
