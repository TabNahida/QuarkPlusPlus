-- cpp-httplib 0.34.0 rejects WebDAV verbs before its routing hooks run.
-- Generate an isolated copy with three additional methods. Do not modify the
-- global xmake package cache; retain the upstream header and MIT license.

function include_directory(target)
    return path.join(target:autogendir(), "rules", "quarkpp", "httplib")
end

function configure(target)
    local owner = target:pkg("cpp-httplib") and target or target:dep("quarkpp_core")
    local package = owner and owner:pkg("cpp-httplib")
    assert(package, "cpp-httplib package is required for the WebDAV adapter")

    local source_path = path.join(package:installdir(), "include", "httplib.h")
    local source = io.readfile(source_path)
    assert(source and source:find('CPPHTTPLIB_VERSION "0.34.0"', 1, true),
        "The WebDAV method patch must be reviewed when upgrading cpp-httplib")
    local old = '"CONNECT", "OPTIONS", "TRACE", "PATCH", "PRI"};'
    local first, last = source:find(old, 1, true)
    assert(first and not source:find(old, last + 1, true), "Unexpected cpp-httplib method allowlist")
    local replacement = '"CONNECT", "OPTIONS", "TRACE", "PATCH", "PRI", "PROPFIND", "MKCOL", "MOVE"};'
    local patched = source:sub(1, first - 1) .. replacement .. source:sub(last + 1)
    -- RFC 9110 forbids Content-Length on 204 responses, including zero. PACE's
    -- strict parser rejects it; keep our WebDAV server and loopback fixtures valid.
    local framing = 'if (res.body.empty() && !res.content_length_ && !res.content_provider_ &&'
    local framing_first, framing_last = patched:find(framing, 1, true)
    assert(framing_first and not patched:find(framing, framing_last + 1, true),
        "Unexpected cpp-httplib empty-response framing")
    patched = patched:sub(1, framing_first - 1) ..
        'if (res.status != 204 && res.body.empty() && !res.content_length_ && !res.content_provider_ &&' ..
        patched:sub(framing_last + 1)
    local directory = include_directory(target)
    local output = path.join(directory, "httplib.h")
    os.mkdir(directory)
    if not os.isfile(output) or io.readfile(output) ~= patched then
        io.writefile(output, patched)
    end
end
