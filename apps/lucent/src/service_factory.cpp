#include "service_factory.hpp"

#include "adapters/application_services.hpp"

namespace lucent::ui {

Expected<Services> create_services() {
    return adapters::create_application_services();
}

}  // namespace lucent::ui
