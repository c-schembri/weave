#include <weave/json.hpp>
#include <weave/log.hpp>
#include <array>

struct Shipment {
  int id;
  double weight;
  bool delivered;
};

int main()
{
  auto shipment = weave::json::read<Shipment>(R"({"id":42,"weight":3.5,"delivered":false})");
  if (!shipment)
    return weave::report_error(shipment.error());

  std::array<char, 256> buffer;
  auto written = weave::json::write(*shipment, std::span{buffer});
  if (!written)
    return weave::report_error(written.error());
  WEAVE_LOG_INFO("%.*s", static_cast<int>(*written), buffer.data());
}
