#include "tune_command.hpp"

#include "cli_util.hpp"
#include "palindrome/image.hpp"
#include "palindrome/video.hpp"

#include <csignal>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace palindrome::cli {
namespace {

// One slider per knob: this list drives both the web UI and the config below,
// so adding a knob is one line here. Ranges/steps are spike guesses to refine.
struct Knob {
  std::string name;
  std::string label;
  double min, max, step, def;
};

const std::vector<Knob> &knobs() {
  static const std::vector<Knob> k = {
      {"cutoff", "Luma cutoff (Hz)", 1.0e6, 5.0e6, 1.0e5, 3.0e6},
      {"sync_cutoff", "Sync LP cutoff (Hz)", 0.3e6, 3.0e6, 1.0e5, 1.2e6},
      {"persistence", "Phosphor persistence (fields)", 0.3, 6.0, 0.1, 1.2},
      {"beam_sigma", "Beam sigma (rows)", 0.0, 2.5, 0.05, 0.8},
      {"gamma", "Gun gamma", 1.0, 3.0, 0.05, 1.0},
      {"sync_level", "Sync slice level", 0.5, 0.95, 0.01, 0.85},
      {"h_kp", "H-hold kp", 0.0, 1.0, 0.02, 1.0},
      {"h_ki", "H-hold ki", 0.0, 5.0e-5, 1.0e-6, 1.0e-5},
      {"h_clamp", "H omega clamp", 0.02, 0.30, 0.01, 0.20},
      {"v_level", "V sync level", 0.1, 0.9, 0.01, 0.4},
      {"v_kp", "V-hold kp", 0.0, 1.0, 0.02, 1.0},
      {"v_ki", "V-hold ki", 0.0, 1.0e-7, 2.0e-9, 2.0e-8},
      {"v_tc", "V integrator (lines)", 0.1, 3.0, 0.1, 0.5},
      {"v_minfield", "V min field frac", 0.0, 0.95, 0.05, 0.7},
  };
  return k;
}

// Output geometry is fixed for the spike (576 dodges the diagonal resonance).
constexpr std::size_t kWidth = 643;
constexpr std::size_t kHeight = 576;
constexpr std::size_t kDecimate = 1;
constexpr std::size_t kBlock = std::size_t{1} << 16;

using Query = std::map<std::string, std::string>;

double knob_value(const Query &q, std::string_view name, double fallback) {
  if (const auto it = q.find(std::string{name}); it != q.end())
    try {
      return std::stod(it->second);
    }
    catch (const std::exception &) {
    }
  return fallback;
}

// Decode the recording with the knob values from `q` and return one encoded PNG
// per field. No caching: the whole pipeline re-runs, as the live path will.
std::vector<std::vector<unsigned char>> render_frames(const LoadedRecording &loaded, const Query &q) {
  const auto g = [&](std::string_view name) {
    for (const auto &kn: knobs())
      if (kn.name == name)
        return knob_value(q, name, kn.def);
    return 0.0;
  };

  const EnvelopeOptions opts{.cutoff_hz = g("cutoff"), .decimation = kDecimate, .no_sound_trap = true, .sound_q = 10.0};
  video::DecoderConfig dc{.sample_rate_hz = loaded.sample_rate_hz / static_cast<double>(kDecimate),
      .width = kWidth,
      .height = kHeight,
      .sync_lp_cutoff_hz = g("sync_cutoff")};
  dc.persistence_fields = g("persistence");
  dc.beam_sigma_rows = g("beam_sigma");
  dc.gamma = g("gamma");
  dc.sep.sync_level = g("sync_level");
  dc.hsweep.pll_kp = g("h_kp");
  dc.hsweep.pll_ki = g("h_ki");
  dc.hsweep.omega_clamp = g("h_clamp");
  dc.vsync.vsync_level = g("v_level");
  dc.vsync.pll_kp = g("v_kp");
  dc.vsync.pll_ki = g("v_ki");
  dc.vsync.integrator_tc_lines = g("v_tc");
  dc.vsync.min_field_fraction = g("v_minfield");

  video::Decoder decoder{dc};
  decoder.prepare(kBlock);
  std::vector<std::vector<unsigned char>> frames;
  const video::Screen::FieldCallback on_field = [&](const video::Screen::Frame &f) {
    frames.push_back(image::encode_png_grey(f.grey, static_cast<unsigned>(f.width), static_cast<unsigned>(f.height)));
  };
  stream_envelope(loaded, opts, [&](std::span<const float> env) { decoder.process(env, on_field); }, kBlock);
  return frames;
}

std::string knobs_json() {
  std::string s = "[";
  for (std::size_t i = 0; i < knobs().size(); ++i) {
    const Knob &k = knobs()[i];
    s += std::format(R"({{"name":"{}","label":"{}","min":{},"max":{},"step":{},"def":{}}})", k.name, k.label, k.min,
        k.max, k.step, k.def);
    if (i + 1 < knobs().size())
      s += ",";
  }
  return s + "]";
}

std::string page() {
  static const std::string tmpl = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<title>PALindrome tune</title><style>
body{font-family:sans-serif;margin:1em;background:#111;color:#ddd}
#wrap{display:flex;gap:1.5em;align-items:flex-start}
.knob{display:flex;align-items:center;gap:.5em;margin:.1em 0}
.knob label{width:15em;font-size:.82em}
.knob input{width:13em}
.knob output{width:6em;text-align:right;font:.8em monospace}
img{background:#000;image-rendering:pixelated;width:643px;height:576px}
button{margin:.4em .4em 0 0}
#status{font:.85em monospace;margin-top:.5em;color:#9c9}
</style></head><body><div id=wrap>
<div><div id=knobs></div>
<div><button id=play>&#9654; play</button>
<input id=scrub type=range min=0 max=0 value=0 style="width:18em"> <span id=fnum>0</span></div>
<div id=status>rendering&hellip;</div></div>
<div><img id=img></div></div>
<script>
const KNOBS=__KNOBS__, vals={}, kd=document.getElementById('knobs');
for(const k of KNOBS){vals[k.name]=k.def;
 const r=document.createElement('div');r.className='knob';
 const l=document.createElement('label');l.textContent=k.label;
 const i=document.createElement('input');i.type='range';i.min=k.min;i.max=k.max;i.step=k.step;i.value=k.def;
 const o=document.createElement('output');o.textContent=(+k.def).toPrecision(4);
 i.addEventListener('input',()=>o.textContent=(+i.value).toPrecision(4));
 i.addEventListener('change',()=>{vals[k.name]=i.value;render();});
 r.append(l,i,o);kd.append(r);}
let count=0,gen=0,playing=null;
const img=document.getElementById('img'),scrub=document.getElementById('scrub'),
 fnum=document.getElementById('fnum'),status=document.getElementById('status');
function show(i){fnum.textContent=i;img.src='/frame?i='+i+'&g='+gen;}
scrub.addEventListener('input',()=>show(+scrub.value));
document.getElementById('play').addEventListener('click',()=>{
 if(playing){clearInterval(playing);playing=null;return;}
 if(!count)return;playing=setInterval(()=>{let i=(+scrub.value+1)%count;scrub.value=i;show(i);},80);});
async function render(){status.textContent='rendering…';
 const qs=Object.entries(vals).map(([k,v])=>k+'='+v).join('&');
 const t=performance.now();
 const j=await(await fetch('/render?'+qs)).json();
 count=j.count;gen++;scrub.max=Math.max(0,count-1);if(+scrub.value>=count)scrub.value=0;
 status.textContent=count+' frames in '+((performance.now()-t)/1000).toFixed(1)+'s';
 show(+scrub.value);}
render();
</script></body></html>)HTML";
  std::string html = tmpl;
  if (const auto p = html.find("__KNOBS__"); p != std::string::npos)
    html.replace(p, 9, knobs_json());
  return html;
}

// --- a dead-simple blocking HTTP/1.1 server: one client at a time, GET only ---

void write_all(int fd, const void *data, std::size_t n) {
  const auto *p = static_cast<const char *>(data);
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w <= 0)
      return;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
}

void respond(int fd, std::string_view status, std::string_view ctype, std::span<const unsigned char> body) {
  const std::string head = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                       "Connection: close\r\n\r\n",
      status, ctype, body.size());
  write_all(fd, head.data(), head.size());
  write_all(fd, body.data(), body.size());
}

void respond_text(int fd, std::string_view status, std::string_view ctype, std::string_view body) {
  respond(fd, status, ctype, {reinterpret_cast<const unsigned char *>(body.data()), body.size()});
}

struct Request {
  std::string path;
  Query query;
};

Request parse_request(std::string_view raw) {
  Request r;
  const auto sp1 = raw.find(' ');
  const auto sp2 = raw.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos)
    return r;
  std::string_view target = raw.substr(sp1 + 1, sp2 - sp1 - 1);
  const auto qm = target.find('?');
  r.path = std::string{target.substr(0, qm)};
  if (qm == std::string_view::npos)
    return r;
  std::string_view qs = target.substr(qm + 1);
  while (!qs.empty()) {
    const auto amp = qs.find('&');
    const std::string_view kv = qs.substr(0, amp);
    if (const auto eq = kv.find('='); eq != std::string_view::npos)
      r.query[std::string{kv.substr(0, eq)}] = std::string{kv.substr(eq + 1)};
    if (amp == std::string_view::npos)
      break;
    qs = qs.substr(amp + 1);
  }
  return r;
}

std::string read_request(int fd) {
  std::string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < (std::size_t{1} << 16)) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0)
      break;
    req.append(buf, static_cast<std::size_t>(n));
  }
  return req;
}

} // namespace

void TuneCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("tune", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Serve an interactive web tuner for the decode/CRT knobs (spike)")
          .add_argument(lyra::opt(port_, "port")["--port"]("Localhost port to serve on"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier override Hz"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to tune (e.g. corpus/wb3_airspy)")));
}

int TuneCommand::run() const {
  const auto loaded = load_recording(recording_, carrier_);

  std::signal(SIGPIPE, SIG_IGN); // a browser closing mid-write must not kill us

  const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    std::println(std::cerr, "tune: socket failed");
    return 1;
  }
  const int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
    std::println(std::cerr, "tune: bind to port {} failed (already in use?)", port_);
    ::close(srv);
    return 1;
  }
  ::listen(srv, 4);
  std::println("tune: serving http://localhost:{}/  (Ctrl-C to stop)", port_);

  std::vector<std::vector<unsigned char>> frames; // most recent decoded batch
  for (;;) {
    const int fd = ::accept(srv, nullptr, nullptr);
    if (fd < 0)
      continue;
    const Request req = parse_request(read_request(fd));
    if (req.path == "/") {
      respond_text(fd, "200 OK", "text/html; charset=utf-8", page());
    }
    else if (req.path == "/render") {
      try {
        frames = render_frames(loaded, req.query);
        respond_text(fd, "200 OK", "application/json",
            std::format(R"({{"count":{},"width":{},"height":{}}})", frames.size(), kWidth, kHeight));
      }
      catch (const std::exception &e) {
        respond_text(fd, "500 Internal Server Error", "text/plain", e.what());
      }
    }
    else if (req.path == "/frame") {
      std::size_t i = frames.size();
      if (const auto it = req.query.find("i"); it != req.query.end())
        try {
          i = std::stoul(it->second);
        }
        catch (const std::exception &) {
        }
      if (i < frames.size())
        respond(fd, "200 OK", "image/png", frames[i]);
      else
        respond_text(fd, "404 Not Found", "text/plain", "no such frame");
    }
    else {
      respond_text(fd, "404 Not Found", "text/plain", "not found");
    }
    ::close(fd);
  }
}

} // namespace palindrome::cli
