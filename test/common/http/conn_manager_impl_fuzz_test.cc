// This fuzzer explores the behavior of HCM with replay of trace actions that describe the behavior
// of a mocked codec and decoder/encoder filters. It is only partially complete (~60% test coverage
// with supplied corpus), since HCM has a lot of behavior to model, requiring investment in building
// out modeling actions and a corpus, which is time consuming and may not have significant security
// of functional correctness payoff beyond existing tests. Places where we could increase fuzz
// coverage include:
// * Watermarks
// * WebSocket upgrades
// * Tracing and stats.
// * Encode filter actions (e.g. modeling stop/continue, only done for decoder today).
// * SSL
// * Idle/drain timeouts.
// * HTTP 1.0 special cases
// * Fuzz config settings
#include "common/common/empty_string.h"
#include "common/http/conn_manager_impl.h"
#include "common/http/context_impl.h"
#include "common/http/date_provider_impl.h"
#include "common/http/exception.h"
#include "common/network/address_impl.h"
#include "common/network/utility.h"
#include "common/stats/symbol_table_creator.h"

#include "test/common/http/conn_manager_impl_common.h"
#include "test/common/http/conn_manager_impl_fuzz.pb.h"
#include "test/fuzz/fuzz_runner.h"
#include "test/fuzz/utility.h"
#include "test/mocks/access_log/mocks.h"
#include "test/mocks/common.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/simulated_time_system.h"

#include "gmock/gmock.h"

using testing::InvokeWithoutArgs;
using testing::Return;

namespace Envoy {
namespace Http {

class FuzzConfig : public ConnectionManagerConfig {
public:
  FuzzConfig()
      : route_config_provider_(time_system_), scoped_route_config_provider_(time_system_),
        stats_{{ALL_HTTP_CONN_MAN_STATS(POOL_COUNTER(fake_stats_), POOL_GAUGE(fake_stats_),
                                        POOL_HISTOGRAM(fake_stats_))},
               "",
               fake_stats_},
        tracing_stats_{CONN_MAN_TRACING_STATS(POOL_COUNTER(fake_stats_))},
        listener_stats_{CONN_MAN_LISTENER_STATS(POOL_COUNTER(fake_stats_))} {
    access_logs_.emplace_back(std::make_shared<NiceMock<AccessLog::MockInstance>>());
  }

  void newStream() {
    codec_ = new NiceMock<MockServerConnection>();
    decoder_filter_ = new NiceMock<MockStreamDecoderFilter>();
    encoder_filter_ = new NiceMock<MockStreamEncoderFilter>();
    EXPECT_CALL(filter_factory_, createFilterChain(_))
        .WillOnce(Invoke([this](FilterChainFactoryCallbacks& callbacks) -> void {
          callbacks.addStreamDecoderFilter(StreamDecoderFilterSharedPtr{decoder_filter_});
          callbacks.addStreamEncoderFilter(StreamEncoderFilterSharedPtr{encoder_filter_});
        }));
    EXPECT_CALL(*decoder_filter_, setDecoderFilterCallbacks(_));
    EXPECT_CALL(*encoder_filter_, setEncoderFilterCallbacks(_));
  }

  // Http::ConnectionManagerConfig
  const std::list<AccessLog::InstanceSharedPtr>& accessLogs() override { return access_logs_; }
  ServerConnectionPtr createCodec(Network::Connection&, const Buffer::Instance&,
                                  ServerConnectionCallbacks&) override {
    return ServerConnectionPtr{codec_};
  }
  DateProvider& dateProvider() override { return date_provider_; }
  std::chrono::milliseconds drainTimeout() override { return std::chrono::milliseconds(100); }
  FilterChainFactory& filterFactory() override { return filter_factory_; }
  bool generateRequestId() override { return true; }
  bool preserveExternalRequestId() const override { return false; }
  uint32_t maxRequestHeadersKb() const override { return max_request_headers_kb_; }
  absl::optional<std::chrono::milliseconds> idleTimeout() const override { return idle_timeout_; }
  std::chrono::milliseconds streamIdleTimeout() const override { return stream_idle_timeout_; }
  std::chrono::milliseconds requestTimeout() const override { return request_timeout_; }
  std::chrono::milliseconds delayedCloseTimeout() const override { return delayed_close_timeout_; }
  Router::RouteConfigProvider* routeConfigProvider() override { return &route_config_provider_; }
  Config::ConfigProvider* scopedRouteConfigProvider() override {
    return &scoped_route_config_provider_;
  }
  const std::string& serverName() override { return server_name_; }
  HttpConnectionManagerProto::ServerHeaderTransformation serverHeaderTransformation() override {
    return server_transformation_;
  }
  ConnectionManagerStats& stats() override { return stats_; }
  ConnectionManagerTracingStats& tracingStats() override { return tracing_stats_; }
  bool useRemoteAddress() override { return use_remote_address_; }
  const Http::InternalAddressConfig& internalAddressConfig() const override {
    return internal_address_config_;
  }
  uint32_t xffNumTrustedHops() const override { return 0; }
  bool skipXffAppend() const override { return false; }
  const std::string& via() const override { return EMPTY_STRING; }
  Http::ForwardClientCertType forwardClientCert() override { return forward_client_cert_; }
  const std::vector<Http::ClientCertDetailsType>& setCurrentClientCertDetails() const override {
    return set_current_client_cert_details_;
  }
  const Network::Address::Instance& localAddress() override { return local_address_; }
  const absl::optional<std::string>& userAgent() override { return user_agent_; }
  const TracingConnectionManagerConfig* tracingConfig() override { return tracing_config_.get(); }
  ConnectionManagerListenerStats& listenerStats() override { return listener_stats_; }
  bool proxy100Continue() const override { return proxy_100_continue_; }
  const Http::Http1Settings& http1Settings() const override { return http1_settings_; }
  bool shouldNormalizePath() const override { return false; }
  bool shouldMergeSlashes() const override { return false; }

  const envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager config_;
  std::list<AccessLog::InstanceSharedPtr> access_logs_;
  MockServerConnection* codec_{};
  MockStreamDecoderFilter* decoder_filter_{};
  MockStreamEncoderFilter* encoder_filter_{};
  NiceMock<MockFilterChainFactory> filter_factory_;
  Event::SimulatedTimeSystem time_system_;
  SlowDateProviderImpl date_provider_{time_system_};
  ConnectionManagerImplHelper::RouteConfigProvider route_config_provider_;
  ConnectionManagerImplHelper::ScopedRouteConfigProvider scoped_route_config_provider_;
  std::string server_name_;
  HttpConnectionManagerProto::ServerHeaderTransformation server_transformation_{
      HttpConnectionManagerProto::OVERWRITE};
  Stats::IsolatedStoreImpl fake_stats_;
  ConnectionManagerStats stats_;
  ConnectionManagerTracingStats tracing_stats_;
  ConnectionManagerListenerStats listener_stats_;
  uint32_t max_request_headers_kb_{Http::DEFAULT_MAX_REQUEST_HEADERS_KB};
  absl::optional<std::chrono::milliseconds> idle_timeout_;
  std::chrono::milliseconds stream_idle_timeout_{};
  std::chrono::milliseconds request_timeout_{};
  std::chrono::milliseconds delayed_close_timeout_{};
  bool use_remote_address_{true};
  Http::ForwardClientCertType forward_client_cert_{Http::ForwardClientCertType::Sanitize};
  std::vector<Http::ClientCertDetailsType> set_current_client_cert_details_;
  Network::Address::Ipv4Instance local_address_{"127.0.0.1"};
  absl::optional<std::string> user_agent_;
  TracingConnectionManagerConfigPtr tracing_config_;
  bool proxy_100_continue_{true};
  bool preserve_external_request_id_{false};
  Http::Http1Settings http1_settings_;
  Http::DefaultInternalAddressConfig internal_address_config_;
  bool normalize_path_{true};
};

// Internal representation of stream state. Encapsulates the stream state, mocks
// and encoders for both the request/response.
class FuzzStream {
public:
  // We track stream state here to prevent illegal operations, e.g. applying an
  // encodeData() to the codec after encodeTrailers(). This is necessary to
  // maintain the preconditions for operations on the codec at the API level. Of
  // course, it's the codecs must be robust to wire-level violations. We
  // explore these violations via MutateAction and SwapAction at the connection
  // buffer level.
  enum class StreamState { PendingHeaders, PendingDataOrTrailers, Closed };

  FuzzStream(ConnectionManagerImpl& conn_manager, FuzzConfig& config,
             const HeaderMap& request_headers, bool end_stream)
      : conn_manager_(conn_manager), config_(config) {
    config_.newStream();
    EXPECT_CALL(*config_.codec_, dispatch(_))
        .WillOnce(InvokeWithoutArgs([this, &request_headers, end_stream] {
          decoder_ = &conn_manager_.newStream(encoder_);
          auto headers = std::make_unique<TestHeaderMapImpl>(request_headers);
          if (headers->Method() == nullptr) {
            headers->setReferenceKey(Headers::get().Method, "GET");
          }
          decoder_->decodeHeaders(std::move(headers), end_stream);
        }));
    fakeOnData();
    decoder_filter_ = config.decoder_filter_;
    encoder_filter_ = config.encoder_filter_;
    request_state_ = end_stream ? StreamState::Closed : StreamState::PendingDataOrTrailers;
    response_state_ = StreamState::PendingHeaders;
  }

  void fakeOnData() {
    Buffer::OwnedImpl fake_input;
    conn_manager_.onData(fake_input, false);
  }

  Http::FilterHeadersStatus fromHeaderStatus(test::common::http::HeaderStatus status) {
    switch (status) {
    case test::common::http::HeaderStatus::HEADER_CONTINUE:
      return Http::FilterHeadersStatus::Continue;
    case test::common::http::HeaderStatus::HEADER_STOP_ITERATION:
      return Http::FilterHeadersStatus::StopIteration;
    default:
      return Http::FilterHeadersStatus::Continue;
    }
  }

  Http::FilterDataStatus fromDataStatus(test::common::http::DataStatus status) {
    switch (status) {
    case test::common::http::DataStatus::DATA_CONTINUE:
      return Http::FilterDataStatus::Continue;
    case test::common::http::DataStatus::DATA_STOP_ITERATION_AND_BUFFER:
      return Http::FilterDataStatus::StopIterationAndBuffer;
    case test::common::http::DataStatus::DATA_STOP_ITERATION_AND_WATERMARK:
      return Http::FilterDataStatus::StopIterationAndWatermark;
    case test::common::http::DataStatus::DATA_STOP_ITERATION_NO_BUFFER:
      return Http::FilterDataStatus::StopIterationNoBuffer;
    default:
      return Http::FilterDataStatus::Continue;
    }
  }

  Http::FilterTrailersStatus fromTrailerStatus(test::common::http::TrailerStatus status) {
    switch (status) {
    case test::common::http::TrailerStatus::TRAILER_CONTINUE:
      return Http::FilterTrailersStatus::Continue;
    case test::common::http::TrailerStatus::TRAILER_STOP_ITERATION:
      return Http::FilterTrailersStatus::StopIteration;
    default:
      return Http::FilterTrailersStatus::Continue;
    }
  }

  void decoderFilterCallbackAction(
      const test::common::http::DecoderFilterCallbackAction& decoder_filter_callback_action) {
    switch (decoder_filter_callback_action.decoder_filter_callback_action_selector_case()) {
    case test::common::http::DecoderFilterCallbackAction::kAddDecodedData: {
      if (request_state_ == StreamState::PendingDataOrTrailers) {
        Buffer::OwnedImpl buf(std::string(
            decoder_filter_callback_action.add_decoded_data().size() % (1024 * 1024), 'a'));
        decoder_filter_->callbacks_->addDecodedData(
            buf, decoder_filter_callback_action.add_decoded_data().streaming());
      }
      break;
    }
    default:
      // Maybe nothing is set?
      break;
    }
  }

  void requestAction(StreamState& state, const test::common::http::RequestAction& request_action) {
    switch (request_action.request_action_selector_case()) {
    case test::common::http::RequestAction::kData: {
      if (state == StreamState::PendingDataOrTrailers) {
        const auto& data_action = request_action.data();
        ON_CALL(*decoder_filter_, decodeData(_, _))
            .WillByDefault(InvokeWithoutArgs([this, &data_action]() -> Http::FilterDataStatus {
              if (data_action.has_decoder_filter_callback_action()) {
                decoderFilterCallbackAction(data_action.decoder_filter_callback_action());
              }
              return fromDataStatus(data_action.status());
            }));
        EXPECT_CALL(*config_.codec_, dispatch(_)).WillOnce(InvokeWithoutArgs([this, &data_action] {
          Buffer::OwnedImpl buf(std::string(data_action.size() % (1024 * 1024), 'a'));
          decoder_->decodeData(buf, data_action.end_stream());
        }));
        fakeOnData();
        state = data_action.end_stream() ? StreamState::Closed : StreamState::PendingDataOrTrailers;
      }
      break;
    }
    case test::common::http::RequestAction::kTrailers: {
      if (state == StreamState::PendingDataOrTrailers) {
        const auto& trailers_action = request_action.trailers();
        ON_CALL(*decoder_filter_, decodeTrailers(_))
            .WillByDefault(
                InvokeWithoutArgs([this, &trailers_action]() -> Http::FilterTrailersStatus {
                  if (trailers_action.has_decoder_filter_callback_action()) {
                    decoderFilterCallbackAction(trailers_action.decoder_filter_callback_action());
                  }
                  return fromTrailerStatus(trailers_action.status());
                }));
        EXPECT_CALL(*config_.codec_, dispatch(_))
            .WillOnce(InvokeWithoutArgs([this, &trailers_action] {
              decoder_->decodeTrailers(std::make_unique<TestHeaderMapImpl>(
                  Fuzz::fromHeaders(trailers_action.headers())));
            }));
        fakeOnData();
        state = StreamState::Closed;
      }
      break;
    }
    case test::common::http::RequestAction::kContinueDecoding: {
      decoder_filter_->callbacks_->continueDecoding();
      break;
    }
    case test::common::http::RequestAction::kThrowDecoderException: {
      if (state == StreamState::PendingDataOrTrailers) {
        EXPECT_CALL(*config_.codec_, dispatch(_)).WillOnce(InvokeWithoutArgs([] {
          throw CodecProtocolException("blah");
        }));
        fakeOnData();
        state = StreamState::Closed;
      }
      break;
    }
    default:
      // Maybe nothing is set or not a request action?
      break;
    }
  }

  void responseAction(StreamState& state,
                      const test::common::http::ResponseAction& response_action) {
    const bool end_stream = response_action.end_stream();
    switch (response_action.response_action_selector_case()) {
    case test::common::http::ResponseAction::kContinueHeaders: {
      if (state == StreamState::PendingHeaders) {
        auto headers = std::make_unique<TestHeaderMapImpl>(
            Fuzz::fromHeaders(response_action.continue_headers()));
        headers->setReferenceKey(Headers::get().Status, "100");
        decoder_filter_->callbacks_->encode100ContinueHeaders(std::move(headers));
      }
      break;
    }
    case test::common::http::ResponseAction::kHeaders: {
      if (state == StreamState::PendingHeaders) {
        auto headers =
            std::make_unique<TestHeaderMapImpl>(Fuzz::fromHeaders(response_action.headers()));
        // The client codec will ensure we always have a valid :status.
        // Similarly, local replies should always contain this.
        try {
          Utility::getResponseStatus(*headers);
        } catch (const CodecClientException&) {
          headers->setReferenceKey(Headers::get().Status, "200");
        }
        decoder_filter_->callbacks_->encodeHeaders(std::move(headers), end_stream);
        state = end_stream ? StreamState::Closed : StreamState::PendingDataOrTrailers;
      }
      break;
    }
    case test::common::http::ResponseAction::kData: {
      if (state == StreamState::PendingDataOrTrailers) {
        Buffer::OwnedImpl buf(std::string(response_action.data() % (1024 * 1024), 'a'));
        decoder_filter_->callbacks_->encodeData(buf, end_stream);
        state = end_stream ? StreamState::Closed : StreamState::PendingDataOrTrailers;
      }
      break;
    }
    case test::common::http::ResponseAction::kTrailers: {
      if (state == StreamState::PendingDataOrTrailers) {
        decoder_filter_->callbacks_->encodeTrailers(
            std::make_unique<TestHeaderMapImpl>(Fuzz::fromHeaders(response_action.trailers())));
        state = StreamState::Closed;
      }
      break;
    }
    default:
      // Maybe nothing is set?
      break;
    }
  }

  void streamAction(const test::common::http::StreamAction& stream_action) {
    switch (stream_action.stream_action_selector_case()) {
    case test::common::http::StreamAction::kRequest: {
      requestAction(request_state_, stream_action.request());
      break;
    }
    case test::common::http::StreamAction::kResponse: {
      responseAction(response_state_, stream_action.response());
      break;
    }
    default:
      // Maybe nothing is set?
      break;
    }
  }

  ConnectionManagerImpl& conn_manager_;
  FuzzConfig& config_;
  StreamDecoder* decoder_{};
  NiceMock<MockStreamEncoder> encoder_;
  MockStreamDecoderFilter* decoder_filter_{};
  MockStreamEncoderFilter* encoder_filter_{};
  StreamState request_state_;
  StreamState response_state_;
};

using FuzzStreamPtr = std::unique_ptr<FuzzStream>;

DEFINE_PROTO_FUZZER(const test::common::http::ConnManagerImplTestCase& input) {
  FuzzConfig config;
  NiceMock<Network::MockDrainDecision> drain_close;
  NiceMock<Runtime::MockRandomGenerator> random;
  Stats::SymbolTablePtr symbol_table(Stats::SymbolTableCreator::makeSymbolTable());
  Http::ContextImpl http_context(*symbol_table);
  NiceMock<Runtime::MockLoader> runtime;
  NiceMock<LocalInfo::MockLocalInfo> local_info;
  NiceMock<Upstream::MockClusterManager> cluster_manager;
  NiceMock<Network::MockReadFilterCallbacks> filter_callbacks;
  std::unique_ptr<Ssl::MockConnectionInfo> ssl_connection;
  bool connection_alive = true;

  ON_CALL(filter_callbacks.connection_, ssl()).WillByDefault(Return(ssl_connection.get()));
  ON_CALL(Const(filter_callbacks.connection_), ssl()).WillByDefault(Return(ssl_connection.get()));
  ON_CALL(filter_callbacks.connection_, close(_))
      .WillByDefault(InvokeWithoutArgs([&connection_alive] { connection_alive = false; }));
  filter_callbacks.connection_.local_address_ =
      std::make_shared<Network::Address::Ipv4Instance>("127.0.0.1");
  filter_callbacks.connection_.remote_address_ =
      std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");

  ConnectionManagerImpl conn_manager(config, drain_close, random, http_context, runtime, local_info,
                                     cluster_manager, nullptr, config.time_system_);
  conn_manager.initializeReadFilterCallbacks(filter_callbacks);

  std::vector<FuzzStreamPtr> streams;

  for (const auto& action : input.actions()) {
    ENVOY_LOG_MISC(trace, "action {} with {} streams", action.DebugString(), streams.size());
    if (!connection_alive) {
      ENVOY_LOG_MISC(trace, "skipping due to dead connection");
      break;
    }

    switch (action.action_selector_case()) {
    case test::common::http::Action::kNewStream: {
      streams.emplace_back(new FuzzStream(conn_manager, config,
                                          Fuzz::fromHeaders(action.new_stream().request_headers()),
                                          action.new_stream().end_stream()));
      break;
    }
    case test::common::http::Action::kStreamAction: {
      const auto& stream_action = action.stream_action();
      if (streams.empty()) {
        break;
      }
      (*std::next(streams.begin(), stream_action.stream_id() % streams.size()))
          ->streamAction(stream_action);
      break;
    }
    default:
      // Maybe nothing is set?
      break;
    }
  }

  filter_callbacks.connection_.dispatcher_.clearDeferredDeleteList();
}

} // namespace Http
} // namespace Envoy
