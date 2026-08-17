#include "vga.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "vgaConfig.h"
#include "copy_words.h"
#include "rasterizer.h"
#include "timing.h"

#define IN_SCAN_RAM  __attribute__((section(".vga_scan_ram")))
#define IN_LOCAL_RAM __attribute__((section(".vga_local_ram")))

#define RAM_CODE __attribute__((section(".ramcode")))

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace vga {

/*******************************************************************************
 * Driver configuration.
 */

static constexpr unsigned
  // Used to adjust size of scan_buffer.
  max_pixels_per_line = 800,
  // Fudge factor: shifts timer-initiated DRQ back in time by this many cycles,
  // to delay DRQ until DMA has started.
  drq_shift_cycles = 2,
  // Fudge factor: how long the shock absorber IRQ should lead the actual start
  // of video IRQ, in cycles.
  shock_absorber_shift_cycles = 20,
  // Amount of pad to place on either side of the working buffer, so that lazy
  // rasterizers can scribble slightly outside the lines -- in words.
  extra_pad_words = 4;

// Common fields used in scanout DMA transfer settings: channel 6 (TIM1_UP),
// very-high priority, single-beat bursts, enabled.
static constexpr std::uint32_t dma_xfer_common_cr =
    (6UL << DMA_SxCR_CHSEL_Pos)
  | DMA_SxCR_PL_1 | DMA_SxCR_PL_0
  | DMA_SxCR_EN;


/*******************************************************************************
 * Driver state.
 */

// A copy of the current Timing, held in RAM for fast access.
static Timing current_timing;

// [0, current_mode.video_end_line).  Updated at front porch interrupt.
static unsigned volatile current_line;

/*
 * The vertical timing state.  This is a Gray code and the bits have meaning.
 * See the inspector functions below.
 */
enum class State {
  blank     = 0b00,
  starting  = 0b01,
  active    = 0b11,
  finishing = 0b10,
};

// Should we be producing a video signal?
inline bool is_displayed_state(State s) {
  return static_cast<unsigned>(s) & 0b10;
}

// Should we be rendering a scanline?
inline bool is_rendered_state(State s) {
  return static_cast<unsigned>(s) & 0b01;
}

// Finally, the actual variable.
static State volatile state;

// This is the DMA source for scan-out, copied from the working buffer during
// pend_sv.  It must be located in DMA-capable RAM, and is aligned to allow for
// word-sized DMA reads.
//
// It contains an extra word's worth of pixels to ensure that we can follow
// every line with an extra transfer to blank the outputs.  The extra pixels
// are blanked after the rasterizer returns.
alignas(std::uint32_t) IN_SCAN_RAM
static Pixel scan_buffer[max_pixels_per_line + sizeof(std::uint32_t)];

// This is the working buffer, the target of the Rasterizer.  Its contents will
// be copied to the scan_buffer during hblank if needed.  It need not be in
// DMA-capable RAM.
//
// It's aligned so we can use a high-speed word copy routine.
//
// It has invisible padding at either end because it makes certain tile
// scrolling algorithms simpler to implement if they need not color precisely
// within the lines.
alignas(std::uint32_t) IN_LOCAL_RAM
static struct {
  std::uint32_t left_pad[extra_pad_words];
  Pixel buffer[max_pixels_per_line];
  std::uint32_t right_pad[extra_pad_words];
} working;

// A description of the contents of the working buffer, produced by the last
// Rasterizer that was applied.  This is used to adjust the output timings.
static Rasterizer::RasterInfo working_buffer_shape;

// When a Rasterizer completes and updates the working buffer, we set this
// flag.  This triggers a copy into the scan buffer at next hblank, at which
// time the flag is cleared.  The copy is conditional because it isn't always
// necessary, e.g. in rasterizer that use line-doubling, so we can save some
// resources by eliding it.
//
// Since this is produced and consumed by interrupts running at a single
// priority, it need not be volatile or atomic.
static bool scan_buffer_needs_update;

// A pre-built DMA_SxCR value to be used to start the next DMA transfer.
// This is set up during hblank based on the working_buffer_shape, and consumed
// at start of active video.
static std::uint32_t next_dma_xfer_cr;
IN_LOCAL_RAM
static bool next_use_timer;

// The head of the linked list of Rasterizer bands.
static Band const *band_list_head;

// A copy of the band we're currently processing.  We copy for several reasons:
// - So that the application may keep its Bands in Flash without a latency
//   penalty on the driver.
// - So that we may mutate it, decrementing the line count.
// - So that the application may rewrite its Bands once rendering starts.
static Band current_band;

// A semaphore used to indicate, to the application, when the driver has
// begun processing the most recently configured band list.  Because the
// driver maintains a copy of a Band, and this copy contains a pointer, it
// is not safe to deallocate or repurpose a list of Bands while the driver
// may be using them.  Instead, clear_band_list does it safely using this
// semaphore.
static std::atomic<bool> band_list_taken{false};


/*******************************************************************************
 * Clock configuration.
 */

static std::uint32_t ahb_prescaler(std::uint32_t divisor) {
  switch (divisor) {
    case 1:   return RCC_SYSCLK_DIV1;
    case 2:   return RCC_SYSCLK_DIV2;
    case 4:   return RCC_SYSCLK_DIV4;
    case 8:   return RCC_SYSCLK_DIV8;
    case 16:  return RCC_SYSCLK_DIV16;
    case 64:  return RCC_SYSCLK_DIV64;
    case 128: return RCC_SYSCLK_DIV128;
    case 256: return RCC_SYSCLK_DIV256;
    default:  return RCC_SYSCLK_DIV512;
  }
}

static std::uint32_t apb_prescaler(std::uint32_t divisor) {
  switch (divisor) {
    case 1:  return RCC_HCLK_DIV1;
    case 2:  return RCC_HCLK_DIV2;
    case 4:  return RCC_HCLK_DIV4;
    case 8:  return RCC_HCLK_DIV8;
    default: return RCC_HCLK_DIV16;
  }
}

static std::uint32_t pllp_bits(std::uint32_t divisor) {
  switch (divisor) {
    case 2:  return RCC_PLLP_DIV2;
    case 4:  return RCC_PLLP_DIV4;
    case 6:  return RCC_PLLP_DIV6;
    default: return RCC_PLLP_DIV8;
  }
}

// Switches the CPU/bus clocks to match cfg, going by way of the internal HSI
// oscillator so that the main PLL can be safely reprogrammed.
static void configure_clocks(ClockConfig const &cfg) {
  // Step down to the 16 MHz internal oscillator; this frees up the PLL.
  RCC_OscInitTypeDef hsi_osc = {};
  hsi_osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  hsi_osc.HSIState = RCC_HSI_ON;
  hsi_osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  hsi_osc.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&hsi_osc);

  RCC_ClkInitTypeDef to_hsi = {};
  to_hsi.ClockType = RCC_CLOCKTYPE_SYSCLK;
  to_hsi.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  HAL_RCC_ClockConfig(&to_hsi, (std::uint32_t) cfg.flash_latency);

  // Reprogram and restart the main PLL from the crystal.
  RCC_OscInitTypeDef pll_osc = {};
  pll_osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  pll_osc.HSEState = RCC_HSE_ON;
  pll_osc.PLL.PLLState = RCC_PLL_ON;
  pll_osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  pll_osc.PLL.PLLM = cfg.crystal_divisor;
  pll_osc.PLL.PLLN = cfg.vco_multiplier;
  pll_osc.PLL.PLLP = pllp_bits(cfg.general_divisor);
  pll_osc.PLL.PLLQ = cfg.pll48_divisor;
  HAL_RCC_OscConfig(&pll_osc);

  // Switch back to the (now reconfigured) PLL with the requested bus dividers.
  RCC_ClkInitTypeDef to_pll = {};
  to_pll.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                    | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  to_pll.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  to_pll.AHBCLKDivider = ahb_prescaler(cfg.ahb_divisor);
  to_pll.APB1CLKDivider = apb_prescaler(cfg.apb1_divisor);
  to_pll.APB2CLKDivider = apb_prescaler(cfg.apb2_divisor);
  HAL_RCC_ClockConfig(&to_pll, (std::uint32_t) cfg.flash_latency);
}


/*******************************************************************************
 * Driver API.
 */

void init() {
  // Turn on I/O compensation cell to reduce noise on power supply.
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  SYSCFG->CMPCR |= SYSCFG_CMPCR_CMP_PD;

  // Turn a bunch of stuff on.
  SYNC_GPIO_CLK_ENABLE();
  VIDEO_GPIO_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  // Configure FIFO: quarter threshold, FIFO enabled (direct mode disabled),
  // FIFO error interrupt off.
  DMA2_Stream5->FCR = DMA_SxFCR_DMDIS;

  // Configure the pixel-generation timer used during reduced-horizontal mode.
  // We use TIM1; it's an APB2 (fast) peripheral, and with our clock config
  // it gets clocked at the full CPU rate.  We'll load ARR under rasterizer
  // control to synthesize 1/n rates.
  __HAL_RCC_TIM1_CLK_ENABLE();
  TIM1->PSC = 0;  // Divide input clock by 1.
  TIM1->CR1 = TIM_CR1_URS;
  TIM1->DIER = TIM_DIER_UDE;  // DRQ on update

  // Configure our interrupt priorities.  The scheme is:
  //  TIM4 (horizontal) gets highest priority.
  //  TIM3 (shock absorber) is set just lower.
  //  PendSV (rendering, user code) is lowest.
  // We could fit other stuff into the gaps later.
  NVIC_SetPriority(TIM4_IRQn, 0);
  NVIC_SetPriority(TIM3_IRQn, 1);
  NVIC_SetPriority(PendSV_IRQn, (1 << __NVIC_PRIO_BITS) - 1);

  // Halt all our timers on debug.
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM4_STOP | DBGMCU_APB1_FZ_DBG_TIM3_STOP;
  DBGMCU->APB2FZ |= DBGMCU_APB2_FZ_DBG_TIM1_STOP;

  // Enable Flash cache and prefetching to try and reduce jitter.
  // This only affects best-effort-level code, not anything realtime.
  __HAL_FLASH_DATA_CACHE_ENABLE();
  __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
  __HAL_FLASH_PREFETCH_BUFFER_ENABLE();

  band_list_head = nullptr;
  band_list_taken = false;

  sync_off();
  video_off();
}

void sync_off() {
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = (1u << HSYNC_PIN) | (1u << VSYNC_PIN);
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(SYNC_GPIO_PORT, &gpio);
}

void video_off() {
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = VIDEO_GPIO_MASK;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(VIDEO_GPIO_PORT, &gpio);
}

void sync_on() {
  // Configure the hsync pin to produce hsync via TIM4 (AF2).
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = (1u << HSYNC_PIN);
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;  // fast_50mhz
  gpio.Alternate = GPIO_AF2_TIM4;
  HAL_GPIO_Init(SYNC_GPIO_PORT, &gpio);

  // Configure the vsync pin as a plain GPIO output.
  gpio.Pin = (1u << VSYNC_PIN);
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SYNC_GPIO_PORT, &gpio);
}

void video_on() {
  // Configure the video output pins for parallel video.
  // Using 100MHz output speed gets slightly sharper transitions than 50MHz.
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = VIDEO_GPIO_MASK;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(VIDEO_GPIO_PORT, &gpio);
}

/*
 * Sets up one of the two horizontal timers, which share almost all of their
 * init code.
 */
static void configure_h_timer(Timing const &timing, TIM_TypeDef *tim) {
  auto apb_cycles_per_pixel = timing.clock_config.apb1_divisor > 1
      ? (timing.cycles_per_pixel * 2 / timing.clock_config.apb1_divisor)
      : timing.cycles_per_pixel;

  tim->PSC = apb_cycles_per_pixel - 1;
  tim->ARR = timing.line_pixels - 1;

  bool negative = timing.hsync_polarity == Timing::Polarity::negative;

#ifdef BOARD2
  // TIM4CH1, PB6
  tim->CCR1 = timing.sync_pixels;
  tim->CCMR1 = (tim->CCMR1 & ~(TIM_CCMR1_CC1S | TIM_CCMR1_OC1M))
             | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1;  // PWM mode 1
  tim->CCER = (tim->CCER & ~TIM_CCER_CC1P)
            | TIM_CCER_CC1E
            | (negative ? TIM_CCER_CC1P : 0);
#else
  // TIM4CH4, PD15
  tim->CCR4 = timing.sync_pixels;
  tim->CCMR2 = (tim->CCMR2 & ~(TIM_CCMR2_CC4S | TIM_CCMR2_OC4M))
             | TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4M_1;  // PWM mode 1
  tim->CCER = (tim->CCER & ~TIM_CCER_CC4P)
            | TIM_CCER_CC4E
            | (negative ? TIM_CCER_CC4P : 0);
#endif

  tim->CCR2 = timing.sync_pixels
            + timing.back_porch_pixels - timing.video_lead;
  tim->CCR3 = timing.sync_pixels
            + timing.back_porch_pixels + timing.video_pixels;
}

void configure_timing(Timing const &timing) {
  // Disable outputs during mode change.
  sync_off();
  video_off();

  // Place the horizontal timers in reset, disabling interrupts.
  NVIC_DisableIRQ(TIM4_IRQn);
  __HAL_RCC_TIM4_FORCE_RESET();
  NVIC_ClearPendingIRQ(TIM4_IRQn);

  NVIC_DisableIRQ(TIM3_IRQn);
  __HAL_RCC_TIM3_FORCE_RESET();
  NVIC_ClearPendingIRQ(TIM3_IRQn);

  // Busy-wait for pending DMA to complete.
  while (DMA2_Stream5->CR & DMA_SxCR_EN) {}

  // No scanout strategy can achieve fewer than 4 cycles per pixel.
  assert(timing.cycles_per_pixel >= 4);
  // Because horizontal timing is managed by timers on the slower APB1 bus,
  // make sure that we can express the (AHB) cycles_per_pixel in APB1 units.
  if (timing.clock_config.apb1_divisor > 1) {
    assert(timing.cycles_per_pixel % (timing.clock_config.apb1_divisor / 2)
           == 0);
  }

  // Switch to new CPU clock settings.
  configure_clocks(timing.clock_config);

  // Bring TIM3/TIM4 back out of reset.
  __HAL_RCC_TIM4_RELEASE_RESET();
  __HAL_RCC_TIM3_RELEASE_RESET();
  __HAL_RCC_TIM4_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();

  // Configure TIM3/4 for horizontal sync generation.
  configure_h_timer(timing, TIM3);
  configure_h_timer(timing, TIM4);

  // Adjust tim3's CC2 value back in time.
  TIM3->CCR2 = TIM3->CCR2 - shock_absorber_shift_cycles;

  // Configure tim3 to distribute its enable signal as its trigger output.
  TIM3->CR2 = (TIM3->CR2 & ~(TIM_CR2_MMS | TIM_CR2_CCDS)) | TIM_CR2_MMS_0;

  // Configure tim4 to trigger from tim3 and run forever.
  TIM4->SMCR = (TIM4->SMCR & ~(TIM_SMCR_TS | TIM_SMCR_SMS))
             | TIM_SMCR_TS_1                       // ITR2 (TIM3)
             | TIM_SMCR_SMS_2 | TIM_SMCR_SMS_1;    // Trigger mode

  // Turn on tim4's interrupts.
  TIM4->DIER = TIM_DIER_CC2IE     // Interrupt at start of active video.
             | TIM_DIER_CC3IE;    // Interrupt at end of active video.

  // Turn on only one of tim3's
  TIM3->DIER = TIM_DIER_CC2IE;    // Interrupt at start of active video.

  // Note: timers still not running.

  switch (timing.vsync_polarity) {
    case Timing::Polarity::positive:
      HAL_GPIO_WritePin(SYNC_GPIO_PORT, 1u << VSYNC_PIN, GPIO_PIN_RESET);
      break;
    case Timing::Polarity::negative:
      HAL_GPIO_WritePin(SYNC_GPIO_PORT, 1u << VSYNC_PIN, GPIO_PIN_SET);
      break;
  }

  // Scribble over working buffer to help catch bugs.
  for (std::size_t i = 0; i < sizeof(working.buffer); i += 2) {
    working.buffer[i] = 0xFF;
    working.buffer[i + 1] = 0x00;
  }

  // Blank the final word of the scan buffer.
  for (unsigned i = 0; i < sizeof(std::uint32_t); ++i) {
    scan_buffer[timing.video_pixels + i] = 0;
  }

  // Set up global state.
  current_line = 0;
  current_timing = timing;
  state = State::blank;
  working_buffer_shape = {
    .offset = 0,
    .length = 0,
    .cycles_per_pixel = timing.cycles_per_pixel,
    .repeat_lines = 0,
  };
  next_use_timer = false;

  scan_buffer_needs_update = false;

  // Start TIM3, which starts TIM4.
  NVIC_EnableIRQ(TIM3_IRQn);
  NVIC_EnableIRQ(TIM4_IRQn);
  TIM3->CR1 |= TIM_CR1_CEN;

  sync_on();
}

void configure_band_list(Band const *head) {
  band_list_head = head;
  band_list_taken = false;
}

void clear_band_list() {
  configure_band_list(nullptr);
  while (!band_list_taken) __WFI();
}

void wait_for_vblank() {
  while (!in_vblank()) __WFI();
}

bool in_vblank() {
  return current_line < current_timing.video_start_line;
}

void sync_to_vblank() {
  while (in_vblank()) __WFI();
  wait_for_vblank();
}

/*******************************************************************************
 * Horizontal timing implementation.  See also the ISR, declared outside of
 * namespace vga toward the end of the file.
 */

RAM_CODE
static void start_of_active_video() {
  // The start-of-active-video (SAV) event is only significant during visible
  // lines.
  if (UNLIKELY(!is_displayed_state(state))) return;

  // Clear stream 5 flags (HIFCR is a write-1-to-clear register).
  DMA2->HIFCR = DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5
              | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;

  // Start the countdown for first DRQ.
  TIM1->CR1 = TIM_CR1_URS | (next_use_timer ? TIM_CR1_CEN : 0);

  DMA2_Stream5->CR = next_dma_xfer_cr;
}

RAM_CODE
static void end_of_active_video() {
  // The end-of-active-video (EAV) event is always significant, as it advances
  // the line state machine and kicks off PendSV.

  // Shut off TIM1; only really matters in reduced-horizontal mode.
  TIM1->CR1 = TIM_CR1_URS;

  // Apply timing changes requested by the last rasterizer.
  TIM4->CCR2 = current_timing.sync_pixels
             + current_timing.back_porch_pixels - current_timing.video_lead
             + working_buffer_shape.offset;

  // Pend a PendSV to process hblank tasks.
  SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;

  // We've finished this line; figure out what to do on the next one.
  unsigned next_line = current_line + 1;

  if (next_line == current_timing.vsync_start_line
      || next_line == current_timing.vsync_end_line) {
    // Either edge of vsync pulse.
    SYNC_GPIO_PORT->ODR ^= (1u << VSYNC_PIN);
  } else if (next_line == std::uint16_t(current_timing.video_start_line - 1)) {
    // We're one line before scanout begins -- need to start rasterizing.
    state = State::starting;
    if (band_list_head) {
      current_band = *band_list_head;
    } else {
      current_band = { nullptr, 0, nullptr };
    }
    band_list_taken = true;
  } else if (next_line == current_timing.video_start_line) {
    // Time to start output.  This will cause PendSV to copy rasterization
    // output into place for scanout, and the next SAV will start DMA.
    state = State::active;
  } else if (next_line == std::uint16_t(current_timing.video_end_line - 1)) {
    // For the final line, suppress rasterization but continue preparing
    // previously rasterized data for scanout, and continue starting DMA in
    // SAV.
    state = State::finishing;
  } else if (next_line == std::uint16_t(current_timing.video_end_line)) {
    // All done!  Suppress all scanout activity.
    state = State::blank;
    next_line = 0;
  }

  current_line = next_line;
}

void default_hblank_interrupt() __attribute__((weak));  // decl hack
RAM_CODE void default_hblank_interrupt() {}


/*******************************************************************************
 * Rasterization interface.  These are implementation factors of the PendSV
 * ISR.
 */

/*
 * Advances the current rasterizer band, possibly switching it for the next if
 * we've reached the end.  The 'edge' parameter is used only in the recursive
 * case.  (It would not appear at all if this language had nested functions.)
 */
RAM_CODE
static bool advance_rasterizer_band(bool edge = false) {
  if (current_band.line_count) {
    --current_band.line_count;
    return edge;
  }

  if (current_band.next) {
    current_band = *current_band.next;
    return advance_rasterizer_band(true);
  } else {
    current_band = { nullptr, 0, nullptr };
    return edge;
  }
}

/*
 * Transfers the contents of the working buffer into the scan buffer, if
 * necessary.
 */
RAM_CODE
static void update_scan_buffer() {
  if (scan_buffer_needs_update) {
    // Flip working_buffer into scan_buffer.  We know its contents are ready
    // because of the scan_buffer_needs_update flag.  Note that the flag may
    // not have been set, even in a displayed state, if we're repeating a
    // line.
    //
    // Note that GCC can't see that we've aligned the buffers correctly, so we
    // have to do a multi-cast dance. :-/
    copy_words(
        reinterpret_cast<std::uint32_t const *>(
          static_cast<void *>(working.buffer)),
        reinterpret_cast<std::uint32_t *>(
          static_cast<void *>(scan_buffer)),
        (working_buffer_shape.length + sizeof(std::uint32_t) - 1)
          / sizeof(std::uint32_t));
    for (unsigned i = 0; i < sizeof(std::uint32_t); ++i) {
      scan_buffer[working_buffer_shape.length + i] = 0;
    }
    scan_buffer_needs_update = false;
  }
}

/*
 * Prepares a configuration for the DMA stream and configures the horizontal
 * timer, if it's relevant to this mode.
 */
RAM_CODE
static void prepare_for_scanout() {
  DMA2_Stream5->CR &= ~DMA_SxCR_EN;

  if (working_buffer_shape.cycles_per_pixel > 4) {
    // Adjust reload frequency of TIM1 to accomodate desired pixel clock.
    // (ARR value is period - 1.)
    TIM1->ARR = working_buffer_shape.cycles_per_pixel - 1;
    // Force an update to reset the timer state.
    TIM1->EGR = TIM_EGR_UG;
    // Configure the timer as *almost* ready to produce a DRQ, less a small
    // value (fudge factor).  Gotta do this after the update event, above,
    // because that clears CNT.
    TIM1->CNT = TIM1->ARR - drq_shift_cycles;
    TIM1->SR = 0;

    DMA2_Stream5->PAR = VIDEO_GPIO_ODR_BYTE;  // Used byte of VIDEO_GPIO ODR
    DMA2_Stream5->M0AR = reinterpret_cast<std::uint32_t>(&scan_buffer);

    // The number of bytes read must exactly match the number of bytes written,
    // or the DMA controller will freak out.  Thus, we must adapt the transfer
    // size to the number of bytes transferred.
    std::uint32_t msize;
    switch (working_buffer_shape.length & 3) {
      case 0:
        msize = DMA_SxCR_MSIZE_1;  // word
        DMA2_Stream5->NDTR = working_buffer_shape.length + sizeof(std::uint32_t);
        break;

      case 2:
        msize = DMA_SxCR_MSIZE_0;  // half-word
        DMA2_Stream5->NDTR = working_buffer_shape.length + sizeof(std::uint16_t);
        break;

      default:
        msize = 0;  // byte
        DMA2_Stream5->NDTR = working_buffer_shape.length + sizeof(std::uint8_t);
        break;
    }

    next_dma_xfer_cr = dma_xfer_common_cr
        | (1UL << DMA_SxCR_DIR_Pos)  // memory-to-peripheral
        | msize
        | DMA_SxCR_MINC;            // psize = byte (0), pinc = false
    next_use_timer = true;

  } else {
    // Note that we're using memory as the peripheral side.
    // This DMA controller is a little odd.
    DMA2_Stream5->PAR = reinterpret_cast<std::uint32_t>(&scan_buffer);
    DMA2_Stream5->M0AR = VIDEO_GPIO_ODR_BYTE;  // Used byte of VIDEO_GPIO ODR

    std::uint32_t psize;
    switch (working_buffer_shape.length & 3) {
      case 0:
        psize = DMA_SxCR_PSIZE_1;  // word
        DMA2_Stream5->NDTR = working_buffer_shape.length / sizeof(std::uint32_t) + 1;
        break;

      case 2:
        psize = DMA_SxCR_PSIZE_0;  // half-word
        DMA2_Stream5->NDTR = working_buffer_shape.length / sizeof(std::uint16_t) + 1;
        break;

      default:
        psize = 0;  // byte
        DMA2_Stream5->NDTR = working_buffer_shape.length / sizeof(std::uint8_t) + 1;
        break;
    }

    next_dma_xfer_cr = dma_xfer_common_cr
        | (2UL << DMA_SxCR_DIR_Pos)  // memory-to-memory
        | psize
        | DMA_SxCR_PINC;            // msize = byte (0), minc = false
    next_use_timer = false;
  }
}

/*
 * Generates pixels for the *next* line, not the currently displaying one.
 */
RAM_CODE
static void rasterize_next_line() {
  auto const &timing = current_timing;
  auto next_line = current_line + 1;
  auto visible_line = next_line - timing.video_start_line;

  bool band_edge = advance_rasterizer_band();
  if (working_buffer_shape.repeat_lines == 0 || band_edge) {
    // Either the last rasterizer has run out of its repeat count and wants
    // to be called again, or we've reached a band edge and are going to call
    // the new rasterizer no matter what the old one wished.
    auto r = current_band.rasterizer;
    if (r) {
      working_buffer_shape = r->rasterize(current_timing.cycles_per_pixel,
                                          visible_line,
                                          working.buffer);
      // Request a rewrite of the scanout buffer during next hblank.
    } else {
      working_buffer_shape = {
        .offset = 0,
        .length = 0,
        .cycles_per_pixel = current_timing.cycles_per_pixel,
        .repeat_lines = 0,
      };
    }
    scan_buffer_needs_update = true;
  } else {  // repeat_lines > 0, not band_edge
    --working_buffer_shape.repeat_lines;
  }
}

}  // namespace vga


/*******************************************************************************
 * ISRs and user interrupt hook
 */

void vga_hblank_interrupt()
  __attribute__((weak, alias("_ZN3vga24default_hblank_interruptEv")));

extern "C" RAM_CODE void TIM3_IRQHandler() {
  // We access this APB2 timer through the bridge on AHB1.  This implies
  // both wait states and resource conflicts with scanout.  Get done fast.
  TIM3->SR = static_cast<std::uint16_t>(~TIM_SR_CC2IF);

  // Idle the processor until preempted by any higher-priority interrupt.
  // This ensures that the M4's D-code bus is available for exception entry.
  // NOTE: this behaves correctly on the M4, but WFI is not guaranteed to
  // actually do anything.
  __WFI();
}

extern "C" RAM_CODE void TIM4_IRQHandler() {
  // We have to clear our interrupt flags, or this will recur.
  auto sr = TIM4->SR;

  if (LIKELY(sr & TIM_SR_CC2IF)) {
    TIM4->SR = static_cast<std::uint16_t>(~TIM_SR_CC2IF);
    vga::start_of_active_video();
    return;
  }

  if (sr & TIM_SR_CC3IF) {
    TIM4->SR = static_cast<std::uint16_t>(~TIM_SR_CC3IF);
    vga::end_of_active_video();
    return;
  }
}

extern "C" RAM_CODE void PendSV_Handler() {
  // PendSV event is triggered shortly after EAV to process lower-priority
  // tasks.

  // First, prepare for scanout from SAV on this line.  This has two purposes:
  // it frees up the rasterization target buffer so that we can overwrite it,
  // and it applies pixel timing choices from the *last* rasterizer run to the
  // scanout machine so that we can replace them as well.
  //
  // This writes to the scanout buffer *and* accesses AHB/APB peripherals, so it
  // *cannot* run concurrently with scanout -- so we do it first, during hblank.
  if (LIKELY(is_displayed_state(vga::state))) {
    vga::update_scan_buffer();
    vga::prepare_for_scanout();
  }

  // Allow the application to do additional work during what's left of hblank.
  vga_hblank_interrupt();

  // Second, rasterize the *next* line, if there's a useful next line.
  // Rasterization can take a while, and may run concurrently with scanout.
  // As a result, we just stash our results in places where the *next* PendSV
  // will find and apply them.
  if (LIKELY(is_rendered_state(vga::state))) {
    vga::rasterize_next_line();
  }
}
