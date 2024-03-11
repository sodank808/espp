#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <driver/gpio_filter.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "base_component.hpp"
#include "task.hpp"

namespace espp {
/// \brief A class to handle a GPIO interrupt
/// \details This class uses the ESP-IDF GPIO interrupt handler to detect
///          GPIO interrupts. It then calls the callback function with the event.
///          It can handle multiple GPIO interrupts and call the appropriate callback
///          for each GPIO interrupt.
///
/// \section interrupt_ex0 Interrupt Example
/// \snippet interrupt_example.cpp interrupt example
class Interrupt : public BaseComponent {
public:
  /// \brief The event for the interrupt
  struct Event {
    uint8_t gpio_num; ///< The GPIO number of the interrupt
    bool active;      ///< Whether the interrupt is active or not (based on the active level)
  };

  /// \brief The active level of the GPIO
  enum class ActiveLevel {
    LOW = 0,  ///< Active low
    HIGH = 1, ///< Active high
  };

  /// \brief The type of interrupt to use for the GPIO
  enum class Type {
    ANY_EDGE = GPIO_INTR_ANYEDGE,      ///< Interrupt on any edge
    RISING_EDGE = GPIO_INTR_POSEDGE,   ///< Interrupt on rising edge
    FALLING_EDGE = GPIO_INTR_NEGEDGE,  ///< Interrupt on falling edge
    LOW_LEVEL = GPIO_INTR_LOW_LEVEL,   ///< Interrupt on low level
    HIGH_LEVEL = GPIO_INTR_HIGH_LEVEL, ///< Interrupt on high level
  };

  typedef std::function<void(const Event &)> event_callback_fn; ///< The callback for the event

  /// \brief The configuration for an interrupt on a GPIO
  /// \details This is used to configure the GPIO interrupt
  struct InterruptConfig {
    int gpio_num;                         ///< GPIO number to for this interrupt
    event_callback_fn callback;           ///< Callback for the interrupt event
    ActiveLevel active_level;             ///< Active level of the GPIO
    Type interrupt_type = Type::ANY_EDGE; ///< Interrupt type to use for the GPIO
    bool pullup_enabled = false;          ///< Whether to enable the pullup resistor
    bool pulldown_enabled = false;        ///< Whether to enable the pulldown resistor
    bool enable_pin_glitch_filter =
        false; ///< Whether to enable the pin glitch filter. NOTE: this
               ///< is only supported on some chips (-C and -S series chips)
  };

  /// \brief The configuration for the interrupt
  struct Config {
    std::vector<InterruptConfig> interrupts; ///< The configuration for the interrupts
    size_t event_queue_size = 10;            ///< The size of the event queue
    Task::BaseConfig task_config;            ///< The configuration for the task
    espp::Logger::Verbosity log_level =
        espp::Logger::Verbosity::WARN; ///< The log level for the interrupt
  };

  /// \brief Constructor
  /// \param config The configuration for the interrupt
  explicit Interrupt(const Config &config)
      : BaseComponent("Interrupt", config.log_level)
      , queue_(xQueueCreate(config.event_queue_size, sizeof(EventData)))
      , interrupts_(config.interrupts) {
    // create the event queue
    if (!queue_) {
      logger_.error("Failed to create event queue");
      return;
    }
    // install the isr service if it hasn't been installed yet
    if (!ISR_SERVICE_INSTALLED) {
      gpio_install_isr_service(0);
      ISR_SERVICE_INSTALLED = true;
    }
    // NOTE: no need for lock here, as we are in the constructor
    for (const auto &interrupt : interrupts_) {
      configure_interrupt(interrupt);
    }
    // now make and start the task
    task_ = espp::Task::make_unique({
        .callback = std::bind(&Interrupt::task_callback, this, std::placeholders::_1,
                              std::placeholders::_2),
        .task_config = config.task_config,
    });
    task_->start();
  }

  /// \brief Destructor
  ~Interrupt() {
    // remove the isr handlers
    {
      std::lock_guard<std::recursive_mutex> lock(interrupt_mutex_);
      for (const auto &args : handler_args_) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(args->gpio_num));
      }
    }
    if (queue_) {
      // send to the event queue to wake up the task
      EventData event_data = {-1};
      xQueueSend(queue_, &event_data, 0);
      // stop the task
      task_->stop();
      // delete the queue
      vQueueDelete(queue_);
    }
#if CONFIG_SOC_GPIO_SUPPORT_PIN_GLITCH_FILTER
    for (const auto &handle : glitch_filter_handles_) {
      // disable the glitch filters
      gpio_glitch_filter_disable(handle);
      // and delete the handle
      gpio_del_glitch_filter(handle);
    }
#endif
    // now delete the handler args
    for (const auto &args : handler_args_) {
      delete args;
    }
  }

  /// \brief Add an interrupt to the interrupt handler
  /// \param interrupt The interrupt to add
  void add_interrupt(const InterruptConfig &interrupt) {
    logger_.info("Adding interrupt for GPIO {}", interrupt.gpio_num);
    std::lock_guard<std::recursive_mutex> lock(interrupt_mutex_);
    interrupts_.push_back(interrupt);
    configure_interrupt(interrupt);
  }

protected:
  // only used by subclasses which want to alter how the ISR configuration,
  // task initialization, etc. is done
  explicit Interrupt(std::string_view name, espp::Logger::Verbosity log_level)
      : BaseComponent(name, log_level) {}

  struct HandlerArgs {
    int gpio_num;
    QueueHandle_t event_queue;
  };

  struct EventData {
    int gpio_num;
  };

  static void isr_handler(void *arg) {
    auto *args = static_cast<HandlerArgs *>(arg);
    EventData event_data = {args->gpio_num};
    xQueueSendFromISR(args->event_queue, &event_data, nullptr);
  }

  bool is_active_level(int gpio_num, ActiveLevel active_level) const {
    auto level = gpio_get_level(static_cast<gpio_num_t>(gpio_num));
    return level == static_cast<int>(active_level);
  }

  bool task_callback(std::mutex &m, std::condition_variable &cv) {
    EventData event_data;
    if (xQueueReceive(queue_, &event_data, portMAX_DELAY)) {
      if (event_data.gpio_num == -1) {
        // we received a stop event, so return true to stop the task
        return true;
      }
      logger_.info("Received interrupt for GPIO {}", event_data.gpio_num);
      std::lock_guard<std::recursive_mutex> lock(interrupt_mutex_);
      // use std::find_if to find the interrupt with the matching gpio_num
      auto predicate = [event_data](const InterruptConfig &interrupt) {
        return interrupt.gpio_num == event_data.gpio_num;
      };
      auto interrupt = std::find_if(interrupts_.begin(), interrupts_.end(), predicate);
      if (interrupt == interrupts_.end()) {
        logger_.error("No interrupt found for GPIO {}", event_data.gpio_num);
        return false;
      }
      if (!interrupt->callback) {
        logger_.error("No callback registered for GPIO {}", event_data.gpio_num);
        return false;
      }
      logger_.debug("Calling interrupt callback for GPIO {}", event_data.gpio_num);
      bool active = is_active_level(event_data.gpio_num, interrupt->active_level);
      logger_.debug("GPIO {} is {}", event_data.gpio_num, active ? "active" : "inactive");
      Event event = {static_cast<uint8_t>(event_data.gpio_num), active};
      interrupt->callback(event);
    }
    // we don't want to stop the task, so return false
    return false;
  }

  void configure_interrupt(const InterruptConfig &interrupt) {
    logger_.info("Configuring interrupt for GPIO {}", interrupt.gpio_num);
    logger_.debug("Config: {}", interrupt);
    if (interrupt.callback == nullptr) {
      logger_.error("No callback provided for GPIO {}, not registering interrupt",
                    interrupt.gpio_num);
      return;
    }
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = 1ULL << interrupt.gpio_num;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = static_cast<gpio_int_type_t>(interrupt.interrupt_type);
    io_conf.pull_up_en = interrupt.pullup_enabled ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en =
        interrupt.pulldown_enabled ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    {
      std::lock_guard<std::recursive_mutex> lock(interrupt_mutex_);
      // add the isr handler
      HandlerArgs *handler_arg = new HandlerArgs{interrupt.gpio_num, queue_};
      handler_args_.push_back(handler_arg);
      gpio_isr_handler_add(static_cast<gpio_num_t>(interrupt.gpio_num), isr_handler,
                           static_cast<void *>(handler_arg));
      // if we need to enable the glitch filter, do so
      if (interrupt.enable_pin_glitch_filter) {
#if CONFIG_SOC_GPIO_SUPPORT_PIN_GLITCH_FILTER
        logger_.info("Enabling glitch filter for GPIO {}", interrupt.gpio_num);
        gpio_glitch_filter_handle_t handle;
        gpio_pin_glitch_filter_config_t filter_config;
        memset(&filter_config, 0, sizeof(filter_config));
        filter_config.gpio_num = static_cast<gpio_num_t>(interrupt.gpio_num);
        auto err = gpio_new_pin_glitch_filter(&filter_config, &handle);
        if (err != ESP_OK) {
          logger_.error("Failed to enable glitch filter for GPIO {}: {}", interrupt.gpio_num,
                        esp_err_to_name(err));
          return;
        }
        // save the handle
        glitch_filter_handles_.push_back(handle);
        // and enable the glitch filter
        gpio_glitch_filter_enable(handle);
#else
        logger_.warn("Glitch filter not supported on this chip");
#endif
      }
    }
  }

  static bool ISR_SERVICE_INSTALLED;

  QueueHandle_t queue_{nullptr};
  std::recursive_mutex interrupt_mutex_;
  std::vector<InterruptConfig> interrupts_;
  std::vector<HandlerArgs *> handler_args_;
  std::vector<gpio_glitch_filter_handle_t> glitch_filter_handles_;
  std::unique_ptr<Task> task_;
};
} // namespace espp

// for printing the interrupt type using libfmt
template <> struct fmt::formatter<espp::Interrupt::Type> {
  constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  template <typename FormatContext> auto format(espp::Interrupt::Type t, FormatContext &ctx) {
    switch (t) {
    case espp::Interrupt::Type::ANY_EDGE:
      return fmt::format_to(ctx.out(), "ANY_EDGE");
    case espp::Interrupt::Type::RISING_EDGE:
      return fmt::format_to(ctx.out(), "RISING_EDGE");
    case espp::Interrupt::Type::FALLING_EDGE:
      return fmt::format_to(ctx.out(), "FALLING_EDGE");
    case espp::Interrupt::Type::LOW_LEVEL:
      return fmt::format_to(ctx.out(), "LOW_LEVEL");
    case espp::Interrupt::Type::HIGH_LEVEL:
      return fmt::format_to(ctx.out(), "HIGH_LEVEL");
    }
    return fmt::format_to(ctx.out(), "UNKNOWN");
  }
};

// for printing the active level using libfmt
template <> struct fmt::formatter<espp::Interrupt::ActiveLevel> {
  constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(espp::Interrupt::ActiveLevel t, FormatContext &ctx) {
    switch (t) {
    case espp::Interrupt::ActiveLevel::LOW:
      return fmt::format_to(ctx.out(), "LOW");
    case espp::Interrupt::ActiveLevel::HIGH:
      return fmt::format_to(ctx.out(), "HIGH");
    }
    return fmt::format_to(ctx.out(), "UNKNOWN");
  }
};

// for printing the InterruptConfig using libfmt
template <> struct fmt::formatter<espp::Interrupt::InterruptConfig> {
  constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const espp::Interrupt::InterruptConfig &t, FormatContext &ctx) {
    return fmt::format_to(ctx.out(),
                          "InterruptConfig{{gpio_num={}, active_level={}, interrupt_type={}, "
                          "pullup_enabled={}, pulldown_enabled={}, enable_pin_glitch_filter={}}}",
                          t.gpio_num, t.active_level, t.interrupt_type, t.pullup_enabled,
                          t.pulldown_enabled, t.enable_pin_glitch_filter);
  }
};
