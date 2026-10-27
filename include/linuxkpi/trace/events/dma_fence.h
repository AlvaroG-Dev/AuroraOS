/* SPDX-License-Identifier: GPL-2.0-only */
/* Puntos de traza de dma_fence: sin trazado en la capa (todo vacio). */
#ifndef _LKPI_TRACE_EVENTS_DMA_FENCE_H
#define _LKPI_TRACE_EVENTS_DMA_FENCE_H

struct dma_fence;

#define trace_dma_fence_init(f) do { (void)(f); } while (0)
#define trace_dma_fence_emit(f) do { (void)(f); } while (0)
#define trace_dma_fence_destroy(f) do { (void)(f); } while (0)
#define trace_dma_fence_enable_signal(f) do { (void)(f); } while (0)
#define trace_dma_fence_signaled(f) do { (void)(f); } while (0)
#define trace_dma_fence_wait_start(f) do { (void)(f); } while (0)
#define trace_dma_fence_wait_end(f) do { (void)(f); } while (0)
#define trace_dma_fence_wait_start_enabled() 0
#define trace_dma_fence_wait_end_enabled() 0
#define trace_dma_fence_init_enabled() 0
#define EXPORT_TRACEPOINT_SYMBOL(name)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name)

#endif
