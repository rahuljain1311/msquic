/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    MsQuic MTU Unittest

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "MtuTest.cpp.clog.h"
#endif

_Function_class_(NEW_CONNECTION_CALLBACK)
static
bool
ListenerAcceptConnection(
    _In_ TestListener* Listener,
    _In_ HQUIC ConnectionHandle
)
{
    ServerAcceptContext* AcceptContext = (ServerAcceptContext*)Listener->Context;
    *AcceptContext->NewConnection = new(std::nothrow) TestConnection(ConnectionHandle);
    if (*AcceptContext->NewConnection == nullptr || !(*AcceptContext->NewConnection)->IsValid()) {
        TEST_FAILURE("Failed to accept new TestConnection.");
        delete* AcceptContext->NewConnection;
        *AcceptContext->NewConnection = nullptr;
        return false;
    }
    if (AcceptContext->ExpectedTransportCloseStatus != QUIC_STATUS_SUCCESS) {
        (*AcceptContext->NewConnection)->SetExpectedTransportCloseStatus(
            AcceptContext->ExpectedTransportCloseStatus);
    }
    CxPlatEventSet(AcceptContext->NewConnectionReady);
    return true;
}

void
QuicTestMtuSettings()
{
    {
        //
        // Test setting on library works
        //
        MsQuicSettings CurrentSettings;
        uint32_t SettingsSize = sizeof(CurrentSettings);
        TEST_QUIC_SUCCEEDED(
            MsQuic->GetParam(
                NULL,
                QUIC_PARAM_LEVEL_GLOBAL,
                QUIC_PARAM_GLOBAL_SETTINGS,
                &SettingsSize,
                &CurrentSettings));

        MsQuicSettings NewSettings;
        NewSettings.SetMinimumMtu(1400).SetMaximumMtu(1400);
        QUIC_STATUS SetSuccess =
            MsQuic->SetParam(
                NULL,
                QUIC_PARAM_LEVEL_GLOBAL,
                QUIC_PARAM_GLOBAL_SETTINGS,
                sizeof(NewSettings),
                &NewSettings);

        MsQuicSettings UpdatedSettings;
        SettingsSize = sizeof(UpdatedSettings);
        QUIC_STATUS GetSuccess =
            MsQuic->GetParam(
                NULL,
                QUIC_PARAM_LEVEL_GLOBAL,
                QUIC_PARAM_GLOBAL_SETTINGS,
                &SettingsSize,
                &UpdatedSettings);

        CurrentSettings.IsSetFlags = 0;
        CurrentSettings.IsSet.MaximumMtu = TRUE;
        CurrentSettings.IsSet.MinimumMtu = TRUE;
        TEST_QUIC_SUCCEEDED(
            MsQuic->SetParam(
                NULL,
                QUIC_PARAM_LEVEL_GLOBAL,
                QUIC_PARAM_GLOBAL_SETTINGS,
                sizeof(CurrentSettings),
                &CurrentSettings));

        TEST_QUIC_SUCCEEDED(SetSuccess);
        TEST_QUIC_SUCCEEDED(GetSuccess);

        TEST_EQUAL(NewSettings.MinimumMtu, UpdatedSettings.MinimumMtu);
        TEST_EQUAL(NewSettings.MaximumMtu, UpdatedSettings.MaximumMtu);
    }

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());
    MsQuicAlpn Alpn("MsQuicTest");
    {
        {
            MsQuicCredentialConfig ClientCredConfig;
            MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientCredConfig);
            TEST_TRUE(ClientConfiguration.IsValid());

            MsQuicSettings Settings;

            //
            // Set out of range, correct order. This should just corce our boundaries.
            //
            Settings.SetMaximumMtu(0xFFFF).SetMinimumMtu(1);

            TEST_QUIC_STATUS(
                QUIC_STATUS_SUCCESS,
                MsQuic->SetParam(
                    ClientConfiguration,
                    QUIC_PARAM_LEVEL_CONFIGURATION,
                    QUIC_PARAM_CONFIGURATION_SETTINGS,
                    sizeof(Settings),
                    &Settings));

            //
            // Set Inverse Order
            //
            Settings.SetMaximumMtu(1300).SetMinimumMtu(1400);

            TEST_QUIC_STATUS(
                QUIC_STATUS_INVALID_PARAMETER,
                MsQuic->SetParam(
                    ClientConfiguration,
                    QUIC_PARAM_LEVEL_CONFIGURATION,
                    QUIC_PARAM_CONFIGURATION_SETTINGS,
                    sizeof(Settings),
                    &Settings));
        }

        MsQuicSettings ServerSettings;
        MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
        TEST_TRUE(ServerConfiguration.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        UniquePtr<TestConnection> Server;
        ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
        Listener.Context = &ServerAcceptCtx;

        const uint16_t MinimumMtu = 1248;

        {
            TestConnection Client(Registration);
            TEST_TRUE(Client.IsValid());
            MsQuicSettings Settings;
            Settings.SetMaximumMtu(1450).SetMinimumMtu(MinimumMtu);

            MsQuicCredentialConfig ClientCredConfig;
            MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
            TEST_TRUE(ClientConfiguration.IsValid());

            //
            // Start client connection.
            //
            TEST_QUIC_SUCCEEDED(
                Client.Start(
                    ClientConfiguration,
                    QuicAddrFamily,
                    QUIC_LOCALHOST_FOR_AF(QuicAddrFamily),
                    ServerLocalAddr.GetPort()));

            //
            // Wait for connection.
            //
            TEST_TRUE(Client.WaitForConnectionComplete());
            TEST_TRUE(Client.GetIsConnected());

            TEST_NOT_EQUAL(nullptr, Server);
            TEST_TRUE(Server->WaitForConnectionComplete());
            TEST_TRUE(Server->GetIsConnected());

            //
            // Set connection settings after open
            //
            Settings.SetMaximumMtu(1400).SetMinimumMtu(1300);
            TEST_QUIC_STATUS(
                QUIC_STATUS_INVALID_PARAMETER,
                Client.SetSettings(Settings));

            QUIC_SETTINGS CheckSettings = Client.GetSettings();
            TEST_EQUAL(1450, CheckSettings.MaximumMtu);
            TEST_EQUAL(MinimumMtu, CheckSettings.MinimumMtu);
        }
    }
}

struct MtuTestContext {
    MsQuicConnection* Connection;

    static QUIC_STATUS ConnCallback(_In_ MsQuicConnection* Conn, _In_opt_ void* Context, _Inout_ QUIC_CONNECTION_EVENT*) {
        (static_cast<MtuTestContext*>(Context))->Connection = Conn;
        return QUIC_STATUS_SUCCESS;
    }
};

static
QUIC_STATISTICS
GetConnStatistics(_In_ MsQuicConnection& Conn) {
    QUIC_STATISTICS value = {};
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            Conn.Handle,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_STATISTICS,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        TEST_FAILURE("MsQuic->GetParam(CONN_STATISTICS) failed, 0x%x.", Status);
    }
    return value;
}

void
QuicMtuDiscoveryTest(
    _In_ int Family,
    _In_ BOOLEAN DropClientProbePackets,
    _In_ BOOLEAN DropServerProbePackets,
    _In_ BOOLEAN RaiseMinimumMtu
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    const uint16_t MinimumMtu = RaiseMinimumMtu ? 1360 : 1280;
    const uint16_t MaximumMtu = 1500;

    MsQuicAlpn Alpn("MsQuicTest");
    MsQuicSettings Settings;
    Settings.SetMinimumMtu(MinimumMtu).SetMaximumMtu(MaximumMtu);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    MtuTestContext Context;
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MtuTestContext::ConnCallback, &Context);
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MtuDropHelper ServerDropper(
        DropServerProbePackets ? MinimumMtu : 0,
        ServerLocalAddr.GetPort(),
        DropClientProbePackets ? MinimumMtu : 0);
    uint16_t ServerExpectedMtu = DropServerProbePackets ? MinimumMtu : MaximumMtu;
    uint16_t ClientExpectedMtu = DropClientProbePackets ? MinimumMtu : MaximumMtu;

    MsQuicConnection Connection(Registration);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());

    TEST_QUIC_SUCCEEDED(Connection.StartLocalhost(ClientConfiguration, ServerLocalAddr));
    TEST_NOT_EQUAL(nullptr, Context.Connection);

    CxPlatSleep(4000); // Wait for the first idle period to expire.

    //
    // Assert our maximum MTUs
    //
    QUIC_STATISTICS ClientStats = GetConnStatistics(Connection);
    QUIC_STATISTICS ServerStats = GetConnStatistics(*Context.Connection);
    Connection.Shutdown(1);
    Context.Connection->Shutdown(1);

    TEST_EQUAL(ClientExpectedMtu, ClientStats.Send.PathMtu);
    TEST_EQUAL(ServerExpectedMtu, ServerStats.Send.PathMtu);
}
