//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2021, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Transport stream processor shared library:
//  Reliable Internet Stream Transport (RIST) input/output plugin for tsp.
//
//----------------------------------------------------------------------------

#include "tsPlatform.h"
TSDUCK_SOURCE;

#if !defined(TS_NO_RIST)
#include "tsAbstractDatagramOutputPlugin.h"
#include "tsPluginRepository.h"
#include <librist/librist.h>


//----------------------------------------------------------------------------
// Encapsulation of common data for input and output plugins
//----------------------------------------------------------------------------

namespace ts {
    class RistPluginData
    {
        TS_NOBUILD_NOCOPY(RistPluginData);
    public:
        // Constructor. Also define commond line arguments.
        RistPluginData(Args*, TSP*);

        // Destructor.
        ~RistPluginData() { cleanup(); }

        // Get command line options.
        bool getOptions(Args*);

        // Add all URL's as peers in the RIST context.
        bool addPeers();

        // Cleanup RIST context.
        void cleanup();

        // Convert between RIST log level to TSDuck severity.
        static int RistLogToSeverity(::rist_log_level level);
        static ::rist_log_level SeverityToRistLog(int severity);

        // Working data.
        ::rist_profile          profile;
        ::rist_ctx*             ctx;
        ::rist_logging_settings log;

    private:
        // Working data.
        TSP*           _tsp;
        uint32_t       _buffer_size;
        int            _encryption_type;
        UString        _secret;
        int            _stats_interval;
        UString        _stats_prefix;
        UStringVector  _peer_urls;
        std::vector<::rist_peer_config*> _peer_configs;

        // A RIST log callback using a RistPluginData* argument.
        static int RistLogCallback(void* arg, ::rist_log_level level, const char* msg);

        // A RIST stats callback using a RistPluginData* argument.
        static int RistStatsCallback(void* arg, const ::rist_stats* stats);
    };
}


//----------------------------------------------------------------------------
// Input/output common data constructor.
//----------------------------------------------------------------------------

ts::RistPluginData::RistPluginData(Args* args, TSP* tsp) :
    profile(RIST_PROFILE_SIMPLE),
    ctx(nullptr),
    log(),
    _tsp(tsp),
    _buffer_size(0),
    _encryption_type(0),
    _secret(),
    _stats_interval(0),
    _stats_prefix(),
    _peer_urls(),
    _peer_configs()
{
    log.log_level = SeverityToRistLog(tsp->maxSeverity());
    log.log_cb = RistLogCallback;
    log.log_cb_arg = this;
    log.log_socket = -1;
    log.log_stream = nullptr;

    args->option(u"", 0, Args::STRING, 1, Args::UNLIMITED_COUNT);
    args->help(u"",
               u"One or more RIST URL's. "
               u"A RIST URL (rist://...) may include tuning parameters in addition to the address and port. "
               u"See https://code.videolan.org/rist/librist/-/wikis/LibRIST%20Documentation for more details.");

    args->option(u"buffer-size", 'b', Args::POSITIVE);
    args->help(u"buffer-size", u"milliseconds",
               u"Default buffer size in milliseconds for packet retransmissions. "
               u"This value overrides the 'buffer=' parameter in the URL.");

    args->option(u"encryption-type", 'e', Enumeration({ // actual value is an AES key size in bits
        {u"AES-128", 128},
        {u"AES-256", 256},
    }));
    args->help(u"encryption-type", u"name",
               u"Specify the encryption type (none by default). "
               u"This value is used when the 'aes-type=' parameter is not present in the URL.");

    args->option(u"profile", 'p', Enumeration({
        {u"simple",   RIST_PROFILE_SIMPLE},
        {u"main",     RIST_PROFILE_MAIN},
        {u"advanced", RIST_PROFILE_ADVANCED},
    }));
    args->help(u"profile", u"name", u"Specify the RIST profile (main profile by default).");

    args->option(u"secret", 's', Args::STRING);
    args->help(u"secret", u"string",
               u"Default pre-shared encryption secret. "
               u"If a pre-shared secret is specified without --encryption-type, AES-128 is used by default. "
               u"This value is used when the 'secret=' parameter is not present in the URL.");

    args->option(u"stats-interval", 0, Args::POSITIVE);
    args->help(u"stats-interval", u"milliseconds",
               u"Periodically report a line of statistics. The interval is in milliseconds. "
               u"The statistics are in JSON format.");

    args->option(u"stats-prefix", 0, Args::STRING);
    args->help(u"stats-prefix", u"'prefix'",
               u"With --stats-interval, specify a prefix to prepend on the statistics line "
               u"before the JSON text to locate the appropriate line in the logs.");

    args->option(u"version", 0, VersionInfo::FormatEnum, 0, 1, true);
    args->help(u"version", u"Display the TSDuck and RIST library version numbers and immediately exits.");
}


//----------------------------------------------------------------------------
// Input/output common data cleanup.
//----------------------------------------------------------------------------

void ts::RistPluginData::cleanup()
{
    // Deallocate all peer configurations (parsed RIST URL's).
    for (size_t i = 0; i < _peer_configs.size(); ++i) {
        if (_peer_configs[i] != nullptr) {
            ::rist_peer_config_free2(&_peer_configs[i]);
            _peer_configs[i] = nullptr;
        }
    }
    _peer_configs.clear();

    // Close the RIST context.
    if (ctx != nullptr) {
        ::rist_destroy(ctx);
        ctx = nullptr;
    }
}


//----------------------------------------------------------------------------
// Input/output common data destructor.
//----------------------------------------------------------------------------

// Get command line options.
bool ts::RistPluginData::getOptions(Args* args)
{
    // Make sure we do not have any allocated resources from librist.
    cleanup();

    // The option --version supplements the TSDuck predefined --version option.
    if (args->present(u"version")) {
        _tsp->info(u"%s\nRIST library: librist version %s, API version %s", {
            VersionInfo::GetVersion(args->intValue(u"version", VersionInfo::Format::LONG)),
            librist_version(),
            librist_api_version()
        });
        ::exit(EXIT_SUCCESS);
    }

    // Normal rist plugin options.
    args->getValues(_peer_urls, u"");
    args->getIntValue(profile, u"profile", RIST_PROFILE_MAIN);
    args->getIntValue(_buffer_size, u"buffer-size");
    args->getIntValue(_encryption_type, u"encryption-type", 0);
    args->getValue(_secret, u"secret");
    args->getIntValue(_stats_interval, u"stats-interval", 0);
    args->getValue(_stats_prefix, u"stats-prefix");

    // Get the UTF-8 version of the pre-shared secret.
    const std::string secret8(_secret.toUTF8());

    // Parse all URL's. The rist_peer_config are allocated by the library.
    _peer_configs.resize(_peer_urls.size());
    for (size_t i = 0; i < _peer_urls.size(); ++i) {

        // Parse the URL.
        _peer_configs[i] = nullptr;
        if (::rist_parse_address2(_peer_urls[i].toUTF8().c_str(), &_peer_configs[i]) != 0 || _peer_configs[i] == nullptr) {
            _tsp->error(u"invalid RIST URL: %s", {_peer_urls[i]});
            cleanup();
            return false;
        }

        // Override URL parameters with command-line options.
        ::rist_peer_config* const peer = _peer_configs[i];
        if (_buffer_size > 0) {
            // Unconditionally override 'buffer='
            peer->recovery_length_max = peer->recovery_length_min = _buffer_size;
        }
        if (!_secret.empty() && peer->secret[0] == '\0') {
            // Override 'secret=' only if not specified in the URL.
            if (secret8.size() >= sizeof(peer->secret)) {
                _tsp->error(u"invalid shared secret, maximum length is %d characters", {sizeof(peer->secret) - 1});
                return false;
            }
            ::memset(peer->secret, 0, sizeof(peer->secret));
            ::memcpy(peer->secret, secret8.data(), secret8.size());
        }
        if (peer->secret[0] != '\0' && peer->key_size == 0) {
            // Override 'aes-type=' if unspecified and a secret is specified (AES-128 by default).
            peer->key_size = _encryption_type == 0 ? 128 : _encryption_type;
        }
        if (peer->secret[0] == '\0' && peer->key_size != 0) {
            _tsp->error(u"AES-%d encryption is specified but the shared secret is missing", {peer->key_size});
            return false;
        }
    }

    return true;
}


//----------------------------------------------------------------------------
// Add all URL's as peers in the RIST context.
//----------------------------------------------------------------------------

bool ts::RistPluginData::addPeers()
{
    // Setup statistics callback if required.
    if (_stats_interval > 0 && ::rist_stats_callback_set(ctx, _stats_interval, RistStatsCallback, this) < 0) {
        _tsp->error(u"error setting statistics callback");
        cleanup();
        return false;
    }

    // Add peers one by one.
    for (size_t i = 0; i < _peer_configs.size(); ++i) {
        ::rist_peer* peer = nullptr;
        if (::rist_peer_create(ctx, &peer, _peer_configs[i]) != 0) {
            _tsp->error(u"error creating peer: %s", {_peer_urls[i]});
            cleanup();
            return false;
        }
    }
    return true;
}


//----------------------------------------------------------------------------
// Bridge between librist and tsduck log systems.
//----------------------------------------------------------------------------

// Convert RIST log level to TSDuck severity.
int ts::RistPluginData::RistLogToSeverity(::rist_log_level level)
{
    switch (level) {
        case RIST_LOG_ERROR:
            return ts::Severity::Error;
        case RIST_LOG_WARN:
            return ts::Severity::Warning;
        case RIST_LOG_NOTICE:
            return ts::Severity::Info;
        case RIST_LOG_INFO:
            return ts::Severity::Verbose;
        case RIST_LOG_DEBUG:
            return ts::Severity::Debug;
        case RIST_LOG_SIMULATE:
            return 2; // debug level 2.
        case RIST_LOG_DISABLE:
        default:
            return 100; // Probably never activated
    }
}

// Convert TSDuck severity to RIST log level.
::rist_log_level ts::RistPluginData::SeverityToRistLog(int severity)
{
    switch (severity) {
        case ts::Severity::Fatal:
        case ts::Severity::Severe:
        case ts::Severity::Error:
            return RIST_LOG_ERROR;
        case ts::Severity::Warning:
            return RIST_LOG_WARN;
        case ts::Severity::Info:
            return RIST_LOG_NOTICE;
        case ts::Severity::Verbose:
            return RIST_LOG_INFO;
        case ts::Severity::Debug:
            return RIST_LOG_DEBUG;
        default:
            return RIST_LOG_DISABLE;
    }
}

// A RIST log callback using a RistPluginData* argument.
int ts::RistPluginData::RistLogCallback(void* arg, ::rist_log_level level, const char* msg)
{
    RistPluginData* data = reinterpret_cast<RistPluginData*>(arg);

    if (data != nullptr && msg != nullptr) {
        UString line;
        line.assignFromUTF8(msg);
        while (!line.empty() && IsSpace(line.back())) {
            line.pop_back();
        }
        data->_tsp->log(RistLogToSeverity(level), line);
    }

    // The returned value is undocumented but seems unused by librist, should have been void.
    return 0;
}


//----------------------------------------------------------------------------
// A RIST stats callback using a RistPluginData* argument.
//----------------------------------------------------------------------------

int ts::RistPluginData::RistStatsCallback(void* arg, const ::rist_stats* stats)
{
    RistPluginData* data = reinterpret_cast<RistPluginData*>(arg);

    if (data != nullptr && stats != nullptr) {
        data->_tsp->info(u"%s%s", {data->_stats_prefix, stats->stats_json});
        ::rist_stats_free(stats);
    }

    // The returned value is undocumented but seems unused by librist, should have been void.
    return 0;
}


//----------------------------------------------------------------------------
// Input plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class RistInputPlugin: public InputPlugin
    {
        TS_NOBUILD_NOCOPY(RistInputPlugin);
    public:
        // Implementation of plugin API
        RistInputPlugin(TSP*);
        virtual bool getOptions() override;
        virtual bool isRealTime() override {return true;}
        virtual bool setReceiveTimeout(MilliSecond timeout) override;
        virtual bool start() override;
        virtual bool stop() override;
        virtual size_t receive(TSPacket*, TSPacketMetadata*, size_t) override;

    private:
        RistPluginData _data;
        MilliSecond    _timeout;       // receive timeout.
        ByteBlock      _buffer;        // data in excess from last input.
        int            _last_qsize;    // last queue size in data blocks.
        bool           _qsize_warned;  // a warning was reporting on heavy queue size.
    };
}

TS_REGISTER_INPUT_PLUGIN(u"rist", ts::RistInputPlugin);


//----------------------------------------------------------------------------
// Input plugin constructor
//----------------------------------------------------------------------------

ts::RistInputPlugin::RistInputPlugin(TSP* tsp_) :
    InputPlugin(tsp_, u"Receive TS packets from Reliable Internet Stream Transport (RIST)", u"[options] url [url...]"),
    _data(this, tsp),
    _timeout(0),
    _buffer(),
    _last_qsize(0),
    _qsize_warned(false)
{
}


//----------------------------------------------------------------------------
// Input get command line options
//----------------------------------------------------------------------------

bool ts::RistInputPlugin::getOptions()
{
    return _data.getOptions(this);
}


//----------------------------------------------------------------------------
// Set receive timeout from tsp.
//----------------------------------------------------------------------------

bool ts::RistInputPlugin::setReceiveTimeout(MilliSecond timeout)
{
    if (timeout > 0) {
        _timeout = timeout;
    }
    return true;
}


//----------------------------------------------------------------------------
// Input start method
//----------------------------------------------------------------------------

bool ts::RistInputPlugin::start()
{
    if (_data.ctx != nullptr) {
        tsp->error(u"already started");
        return false;
    }

    // Clear internal state.
    _buffer.clear();
    _last_qsize = 0;
    _qsize_warned = false;

    // Initialize the RIST context.
    tsp->debug(u"calling rist_receiver_create, profile: %d", {_data.profile});
    if (::rist_receiver_create(&_data.ctx, _data.profile, &_data.log) != 0) {
        tsp->error(u"error in rist_receiver_create");
        return false;
    }

    // Add all peers to the RIST context.
    if (!_data.addPeers()) {
        return false;
    }

    // Start reception.
    tsp->debug(u"calling rist_start");
    if (::rist_start(_data.ctx) != 0) {
        tsp->error(u"error starting RIST reception");
        _data.cleanup();
        return false;
    }

    return true;
}


//----------------------------------------------------------------------------
// Input stop method
//----------------------------------------------------------------------------

bool ts::RistInputPlugin::stop()
{
    _data.cleanup();
    return true;
}


//----------------------------------------------------------------------------
// Input method
//----------------------------------------------------------------------------

size_t ts::RistInputPlugin::receive(TSPacket* pkt_buffer, TSPacketMetadata* pkt_data, size_t max_packets)
{
    size_t pkt_count = 0;

    if (!_buffer.empty()) {
        // There are remaining data from a previous receive in the buffer.
        tsp->debug(u"read data from remaining %d bytes in the buffer", {_buffer.size()});
        assert(_buffer.size() % PKT_SIZE == 0);
        pkt_count = std::min(_buffer.size() / PKT_SIZE, max_packets);
        ::memcpy(pkt_buffer->b, _buffer.data(), pkt_count * PKT_SIZE);
        _buffer.erase(0, pkt_count * PKT_SIZE);
    }
    else {
        // Read one data block. Allocated in the library, must be freed later.
        ::rist_data_block* dblock = nullptr;

        // There is no blocking read. Only a timed read with zero meaning "no wait".
        // Here, we poll every few seconds when no timeout is specified and check for abort.
        for (;;) {
            // The returned value is: number of buffers remaining on queue +1 (0 if no buffer returned), -1 on error.
            const int queue_size = ::rist_receiver_data_read2(_data.ctx, &dblock, _timeout == 0 ? 5000 : int(_timeout));
            if (queue_size < 0) {
                tsp->error(u"reception error");
                return 0;
            }
            else if (queue_size == 0 || dblock == nullptr) {
                // No data block returned but not an error, must be a timeout.
                if (_timeout > 0) {
                    // This is a user-specified timeout.
                    tsp->error(u"reception timeout");
                    return 0;
                }
                else if (tsp->aborting()) {
                    // User abort was requested.
                    return 0;
                }
                tsp->debug(u"no packet, queue size: %d, data block: 0x%X, polling librist again", {queue_size, size_t(dblock)});
            }
            else {
                // Report excessive queue size to diagnose reception issues.
                if (queue_size > _last_qsize + 10) {
                    tsp->warning(u"RIST receive queue heavy load: %d data blocks, flow id %d", {queue_size, dblock->flow_id});
                    _qsize_warned = true;
                }
                else if (_qsize_warned && queue_size == 1) {
                    tsp->info(u"RIST receive queue back to normal");
                    _qsize_warned = false;
                }
                _last_qsize = queue_size;

                // Assume that we receive an integral number of TS packets.
                const size_t total_pkt_count = dblock->payload_len / PKT_SIZE;
                const uint8_t* const data_addr = reinterpret_cast<const uint8_t*>(dblock->payload);
                const size_t data_size = total_pkt_count * PKT_SIZE;
                if (data_size < dblock->payload_len) {
                    tsp->warning(u"received %'d bytes, not a integral number of TS packets, %d trailing bytes, first received byte: 0x%X, first trailing byte: 0x%X",
                                 {dblock->payload_len, dblock->payload_len % PKT_SIZE, data_addr[0], data_addr[data_size]});
                }

                // Return the packets which fit in the caller's buffer.
                pkt_count = std::min(total_pkt_count, max_packets);
                ::memcpy(pkt_buffer->b, data_addr, pkt_count * PKT_SIZE);

                // Copy the rest, if any, in the local buffer.
                if (pkt_count < total_pkt_count) {
                    _buffer.copy(data_addr + (pkt_count * PKT_SIZE), (total_pkt_count - pkt_count) * PKT_SIZE);
                }

                // Free returned data block.
                ::rist_receiver_data_block_free2(&dblock);

                // Abort polling loop.
                break;
            }
        }
    }
    return pkt_count;
}


//----------------------------------------------------------------------------
// Output plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class RistOutputPlugin: public AbstractDatagramOutputPlugin
    {
        TS_NOBUILD_NOCOPY(RistOutputPlugin);
    public:
        // Implementation of plugin API
        RistOutputPlugin(TSP*);
        virtual bool getOptions() override;
        virtual bool isRealTime() override {return true;}
        virtual bool start() override;
        virtual bool stop() override;

    protected:
        // Implementation of AbstractDatagramOutputPlugin.
        virtual bool sendDatagram(const void* address, size_t size) override;

    private:
        RistPluginData _data;
        bool           _npd;   // null packet deletion
    };
}

TS_REGISTER_OUTPUT_PLUGIN(u"rist", ts::RistOutputPlugin);


//----------------------------------------------------------------------------
// Output plugin constructor
//----------------------------------------------------------------------------

ts::RistOutputPlugin::RistOutputPlugin(TSP* tsp_) :
    AbstractDatagramOutputPlugin(tsp_, u"Send TS packets using Reliable Internet Stream Transport (RIST)", u"[options] url [url...]", NONE),
    _data(this, tsp),
    _npd(false)
{
    option(u"null-packet-deletion", 'n');
    help(u"null-packet-deletion", u"Enable null packet deletion. The receiver needs to support this.");
}


//----------------------------------------------------------------------------
// Output get command line options
//----------------------------------------------------------------------------

bool ts::RistOutputPlugin::getOptions()
{
    _npd = present(u"null-packet-deletion");
    return _data.getOptions(this) && AbstractDatagramOutputPlugin::getOptions();
}


//----------------------------------------------------------------------------
// Output start method
//----------------------------------------------------------------------------

bool ts::RistOutputPlugin::start()
{
    if (_data.ctx != nullptr) {
        tsp->error(u"already started");
        return false;
    }

    // Initialize the superclass.
    if (!AbstractDatagramOutputPlugin::start()) {
        return false;
    }

    // Initialize the RIST context.
    tsp->debug(u"calling rist_sender_create, profile: %d", {_data.profile});
    if (::rist_sender_create(&_data.ctx, _data.profile, 0, &_data.log) != 0) {
        tsp->error(u"error in rist_sender_create");
        return false;
    }

    // Add null packet deletion option if requested.
    if (_npd && ::rist_sender_npd_enable(_data.ctx) < 0) {
        tsp->error(u"error setting null-packet deletion");
        _data.cleanup();
        return false;
    }

    // Add all peers to the RIST context.
    if (!_data.addPeers()) {
        return false;
    }

    // Start transmission.
    tsp->debug(u"calling rist_start");
    if (::rist_start(_data.ctx) != 0) {
        tsp->error(u"error starting RIST transmission");
        _data.cleanup();
        return false;
    }

    return true;
}


//----------------------------------------------------------------------------
// Output stop method
//----------------------------------------------------------------------------

bool ts::RistOutputPlugin::stop()
{
    // Let the superclass send trailing data, if any.
    AbstractDatagramOutputPlugin::stop();

    // Close RIST communication.
    _data.cleanup();
    return true;
}


//----------------------------------------------------------------------------
// Output method
//----------------------------------------------------------------------------

bool ts::RistOutputPlugin::sendDatagram(const void* address, size_t size)
{
    // Build a RIST data block describing the data to send.
    ::rist_data_block dblock;
    TS_ZERO(dblock);
    dblock.payload = address;
    dblock.payload_len = size;

    // Send the RIST message.
    const int sent = ::rist_sender_data_write(_data.ctx, &dblock);
    if (sent < 0) {
        tsp->error(u"error sending data to RIST");
        return false;
    }
    else if (size_t(sent) != size) {
        // Don't really know what to do, retry with the rest?
        tsp->warning(u"sent %d bytes to RIST, only %d were written", {size, sent});
    }
    return true;
}

#endif // TS_NO_RIST
