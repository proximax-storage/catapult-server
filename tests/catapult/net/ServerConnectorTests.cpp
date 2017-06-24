#include "catapult/net/ServerConnector.h"
#include "catapult/crypto/KeyPair.h"
#include "catapult/ionet/Node.h"
#include "catapult/ionet/PacketSocket.h"
#include "catapult/net/AsyncTcpServer.h"
#include "catapult/net/VerifyPeer.h"
#include "catapult/thread/IoServiceThreadPool.h"
#include "tests/test/core/AddressTestUtils.h"
#include "tests/test/core/ThreadPoolTestUtils.h"
#include "tests/test/net/ClientSocket.h"
#include "tests/test/net/NodeTestUtils.h"
#include "tests/test/net/SocketTestUtils.h"
#include "tests/test/net/mocks/MockAsyncTcpServerAcceptContext.h"

using catapult::mocks::MockAsyncTcpServerAcceptContext;

namespace catapult { namespace net {

	TEST(ServerConnectorTests, InitiallyNoConnectionsAreActive) {
		// Act:
		auto pPool = test::CreateStartedIoServiceThreadPool();
		auto pConnector = CreateServerConnector(std::move(pPool), test::GenerateKeyPair(), ConnectionSettings());

		// Assert:
		EXPECT_EQ(0u, pConnector->numActiveConnections());
	}

	namespace {
		struct ConnectorTestContext {
		public:
			explicit ConnectorTestContext(const ConnectionSettings& settings = ConnectionSettings())
					: ServerKeyPair(test::GenerateKeyPair())
					, ClientKeyPair(test::GenerateKeyPair())
					, pPool(test::CreateStartedIoServiceThreadPool())
					, Service(pPool->service())
					, pConnector(CreateServerConnector(pPool, ClientKeyPair, settings))
			{}

			~ConnectorTestContext() {
				pConnector->shutdown();
				test::WaitForUnique(pConnector, "pConnector");

				CATAPULT_LOG(debug) << "waiting for pool in ConnectorTestContext to drain";
				pPool->join();
			}

		public:
			crypto::KeyPair ServerKeyPair;
			crypto::KeyPair ClientKeyPair;
			std::shared_ptr<thread::IoServiceThreadPool> pPool;
			boost::asio::io_service& Service;
			std::shared_ptr<ServerConnector> pConnector;

		public:
			ionet::Node serverNode() const {
				return test::CreateLocalHostNode(ServerKeyPair.publicKey());
			}

			void waitForActiveConnections(uint32_t numConnections) const {
				WAIT_FOR_VALUE_EXPR(pConnector->numActiveConnections(), numConnections);
			}
		};
	}

	TEST(ServerConnectorTests, ConnectFailsOnConnectError) {
		// Arrange:
		ConnectorTestContext context;
		std::atomic<size_t> numCallbacks(0);

		// Act: try to connect to a server that isn't running
		PeerConnectResult result;
		std::shared_ptr<ionet::PacketSocket> pSocket;
		context.pConnector->connect(context.serverNode(), [&](auto connectResult, const auto& pConnectedSocket) {
			result = connectResult;
			pSocket = pConnectedSocket;
			++numCallbacks;
		});
		WAIT_FOR_ONE(numCallbacks);

		// Assert:
		EXPECT_EQ(PeerConnectResult::Socket_Error, result);
		EXPECT_FALSE(!!pSocket);
		EXPECT_EQ(0u, context.pConnector->numActiveConnections());
	}

	TEST(ServerConnectorTests, ConnectFailsOnVerifyError) {
		// Arrange:
		ConnectorTestContext context;
		std::atomic<size_t> numCallbacks(0);

		// Act: start a server and client verify operation
		PeerConnectResult result;
		test::SpawnPacketServerWork(context.Service, [&](const auto& pSocket) -> void {
			// - trigger a verify error by closing the socket without responding
			pSocket->close();
			++numCallbacks;
		});

		std::shared_ptr<ionet::PacketSocket> pSocket;
		context.pConnector->connect(context.serverNode(), [&](auto connectResult, const auto& pConnectedSocket) -> void {
			result = connectResult;
			pSocket = pConnectedSocket;
			++numCallbacks;
		});

		// - wait for both callbacks to complete and the connection to close
		WAIT_FOR_VALUE(numCallbacks, 2u);
		context.waitForActiveConnections(0);

		// Assert: the verification should have failed and all connections should have been destroyed
		EXPECT_EQ(PeerConnectResult::Verify_Error, result);
		EXPECT_FALSE(!!pSocket);
		EXPECT_EQ(0u, context.pConnector->numActiveConnections());
	}

	namespace {
		struct MultiConnectionState {
			std::vector<PeerConnectResult> Results;
			std::vector<std::shared_ptr<ionet::PacketSocket>> ServerSockets;
			std::vector<std::shared_ptr<ionet::PacketSocket>> ClientSockets;
		};

		MultiConnectionState SetupMultiConnectionTest(const ConnectorTestContext& context, size_t numConnections) {
			// Act: start multiple server and client verify operations
			MultiConnectionState state;
			for (auto i = 0u; i < numConnections; ++i) {
				std::atomic<size_t> numCallbacks(0);
				test::SpawnPacketServerWork(context.Service, [&](const auto& pSocket) -> void {
					state.ServerSockets.push_back(pSocket);
					VerifyClient(pSocket, context.ServerKeyPair, [&](auto, const auto&) {
						++numCallbacks;
					});
				});

				context.pConnector->connect(context.serverNode(), [&](auto connectResult, const auto& pSocket) {
					state.Results.push_back(connectResult);
					state.ClientSockets.push_back(pSocket);
					++numCallbacks;
				});

				// - wait for both verifications to complete
				WAIT_FOR_VALUE(numCallbacks, 2u);
			}

			return state;
		}

		using ResultServerClientHandler = std::function<void (
				PeerConnectResult,
				std::shared_ptr<ionet::PacketSocket>&,
				std::shared_ptr<ionet::PacketSocket>&)>;

		void RunConnectedSocketTest(const ConnectorTestContext& context, const ResultServerClientHandler& handler) {
			// Act: establish a single connection
			auto state = SetupMultiConnectionTest(context, 1);

			// Assert: call the handler
			handler(state.Results.back(), state.ServerSockets.back(), state.ClientSockets.back());
		}
	}

	TEST(ServerConnectorTests, ConnectSucceedsOnVerifySuccess) {
		// Act:
		ConnectorTestContext context;
		RunConnectedSocketTest(context, [&](auto result, const auto&, const auto& pClientSocket) {
			// Assert: the verification should have succeeded and the connection should be active
			EXPECT_EQ(PeerConnectResult::Accepted, result);
			EXPECT_EQ(1u, context.pConnector->numActiveConnections());
			EXPECT_TRUE(!!pClientSocket);
		});
	}

	TEST(ServerConnectorTests, ShutdownClosesConnectedSocket) {
		// Act:
		ConnectorTestContext context;
		RunConnectedSocketTest(context, [&](auto, const auto&, const auto& pClientSocket) {
			// Act: shutdown the connector
			context.pConnector->shutdown();

			// Assert: the client socket was closed
			EXPECT_FALSE(test::IsSocketOpen(*pClientSocket));
			EXPECT_EQ(0u, context.pConnector->numActiveConnections());
		});
	}

	TEST(ServerConnectorTests, CanManageMultipleConnections) {
		// Act: establish multiple connections
		static const auto Num_Connections = 5u;
		ConnectorTestContext context;
		auto state = SetupMultiConnectionTest(context, Num_Connections);

		// Assert: all connections are active
		EXPECT_EQ(Num_Connections, state.Results.size());
		for (auto i = 0u; i < Num_Connections; ++i) {
			EXPECT_EQ(PeerConnectResult::Accepted, state.Results[i]);
			EXPECT_TRUE(!!state.ClientSockets[i]);
		}

		EXPECT_EQ(Num_Connections, context.pConnector->numActiveConnections());
	}

	namespace {
		void RunConnectingSocketTest(const ConnectorTestContext& context, const std::function<void ()>& handler) {
			std::atomic<size_t> numCallbacks(0);

			// Act: start a verify operation that the server does not respond to
			std::shared_ptr<ionet::PacketSocket> pServerSocket;
			test::SpawnPacketServerWork(context.Service, [&](const auto& pSocket) {
				pServerSocket = pSocket;
				++numCallbacks;
			});

			// - (use a result shared_ptr so that the connect callback is valid even after this function returns)
			auto pResult = std::make_shared<PeerConnectResult>(static_cast<PeerConnectResult>(-1));
			context.pConnector->connect(context.serverNode(), [&, pResult](auto connectResult, const auto&) {
				// note that this is not expected to get called until shutdown because the client doesn't read
				// or write any data
				*pResult = connectResult;
			});

			// - wait for the initial work to complete and the connection to become active
			WAIT_FOR_ONE(numCallbacks);
			context.waitForActiveConnections(1);

			// Assert: the client connect handler was never called
			EXPECT_EQ(static_cast<PeerConnectResult>(-1), *pResult);

			// - call the test handler
			handler();
		}
	}

	TEST(ServerConnectorTests, VerifyingConnectionIsIncludedInNumActiveConnections) {
		// Act:
		ConnectorTestContext context;
		RunConnectingSocketTest(context, [&]() {
			// Assert: the verifying connection is active
			EXPECT_EQ(1u, context.pConnector->numActiveConnections());
		});
	}

	TEST(ServerConnectorTests, ShutdownClosesVerifyingSocket) {
		// Act:
		ConnectorTestContext context;
		RunConnectingSocketTest(context, [&]() {
			// Act: shutdown the connector
			context.pConnector->shutdown();

			// Assert: the verifying socket is no longer active
			EXPECT_EQ(0u, context.pConnector->numActiveConnections());
		});
	}

	// region timeout

	namespace {
		bool RunTimeoutTestIteration(const ConnectionSettings& settings, size_t numDesiredActiveConnections) {
			// Arrange:
			ConnectorTestContext context(settings);
			std::atomic<size_t> numCallbacks(0);
			std::atomic<size_t> numDummyConnections(0);

			// Act: start a verify operation that the server does not respond to
			// - server: accept a single connection
			CATAPULT_LOG(debug) << "starting async accept";
			auto pAcceptor = test::CreateLocalHostAcceptor(context.Service);
			auto serverSocket = boost::asio::ip::tcp::socket(context.Service);
			pAcceptor->async_accept(serverSocket, [&numCallbacks](const auto& acceptEc) {
				CATAPULT_LOG(debug) << "async_accept completed with: " << acceptEc.message();
				++numCallbacks;
			});

			// - client: start a connection to the server
			PeerConnectResult result;
			size_t numActiveConnections = 0;
			std::shared_ptr<ionet::PacketSocket> pClientSocket;
			context.pConnector->connect(context.serverNode(), [&](auto connectResult, const auto& pSocket) -> void {
				// - note that any active connections will not be destroyed until the completion of this callback
				numActiveConnections = context.pConnector->numActiveConnections();

				result = connectResult;
				pClientSocket = pSocket;
				++numCallbacks;

				// - if the connect callback is called first, the request likely timed out during connect
				if (numCallbacks < 2) {
					++numDummyConnections;

					// - cancel all outstanding acceptor operations to allow the server to shutdown
					CATAPULT_LOG(debug) << "cancelling outstanding acceptor operations";
					pAcceptor->cancel();
				}
			});

			// - wait for both callbacks to be called
			WAIT_FOR_VALUE(numCallbacks, 2u);

			// Retry: if there are an unexpected number of connections or dummy connections
			if (numActiveConnections != numDesiredActiveConnections || numDummyConnections == numDesiredActiveConnections) {
				CATAPULT_LOG(warning) << "unexpected number of connections " << numActiveConnections
						<< " or dummy connections " << numDummyConnections;
				return false;
			}

			// Assert: the client connect handler was called with a timeout and nullptr
			EXPECT_EQ(PeerConnectResult::Timed_Out, result);
			EXPECT_FALSE(!!pClientSocket);

			// - wait for all connections to be destroyed
			context.waitForActiveConnections(0);
			return true;
		}

		void RunTimeoutTest(const ConnectionSettings& settings, size_t numDesiredActiveConnections) {
			// Assert: non-deterministic because a socket could connect before it times out and/or timeout in the
			//         wrong state (connecting vs verifying)
			test::RunNonDeterministicTest("Timeout", [&]() {
				return RunTimeoutTestIteration(settings, numDesiredActiveConnections);
			});
		}
	}

	TEST(ServerConnectorTests, TimeoutClosesConnectingSocket) {
		// Arrange: timeout immediately (during connect where 0 active connections are expected)
		const auto Num_Expected_Active_Connections = 0;
		ConnectionSettings settings;
		settings.Timeout = utils::TimeSpan::FromMilliseconds(0);

		// Assert:
		RunTimeoutTest(settings, Num_Expected_Active_Connections);
	}

	TEST(ServerConnectorTests, TimeoutClosesVerifyingSocket) {
		// Arrange: timeout with some delay (during verify where 1 active connection is expected)
		const auto Num_Expected_Active_Connections = 1;
		ConnectionSettings settings;
		settings.Timeout = utils::TimeSpan::FromMilliseconds(50);

		// Assert:
		RunTimeoutTest(settings, Num_Expected_Active_Connections);
	}

	// endregion
}}