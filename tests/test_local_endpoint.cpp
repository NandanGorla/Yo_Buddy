#include <catch2/catch_test_macros.hpp>

#include "fake_transport.h"
#include "providers/local_endpoint.h"

using namespace buddy;
using namespace buddy::test;

TEST_CASE("local discovery reads models and llama-server capabilities", "[local]") {
    FakeTransport transport;
    transport.content_type = "application/json";
    transport.chunks = {R"({"data":[{"id":"gemma4:12b","architecture":{"input_modalities":["text","vision"]}}]})"};
    LocalEndpointConnector connector(transport);

    const LocalDiscovery result = connector.discover("http://127.0.0.1:9931/v1", {});
    REQUIRE(result.ok);
    CHECK(result.server == LocalServerKind::LlamaServer);
    REQUIRE(result.models.size() == 1);
    CHECK(result.models[0].id == "gemma4:12b");
    CHECK(result.models[0].vision == true);
    CHECK(transport.calls == 2); // /v1/models followed by llama-server's /props probe
}

TEST_CASE("local discovery refuses remote endpoints before connecting", "[local]") {
    FakeTransport transport;
    LocalEndpointConnector connector(transport);

    const LocalDiscovery result = connector.discover("https://models.example/v1", {});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("loopback") != std::string::npos);
    CHECK(transport.calls == 0);
}

TEST_CASE("local model test sends a tiny image through the shared provider", "[local]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("OK"), finish_chunk("stop"), done_marker()};
    LocalEndpointConnector connector(transport);

    const ChatResult result = connector.test_model("http://localhost:9931/v1", "vision-model", {});
    CHECK(result.status == ChatStatus::Ok);
    CHECK(transport.last_request.url == "http://localhost:9931/v1/chat/completions");
    CHECK(transport.last_request.body.find("\"image_url\"") != std::string::npos);
}
