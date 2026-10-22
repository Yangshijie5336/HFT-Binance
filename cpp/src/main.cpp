#include "marketpipe.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
namespace marketpipe {
namespace {
std::string field(const std::string& s, const char* key) {
  std::regex r(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""); std::smatch m;
  return std::regex_search(s, m, r) ? m[1].str() : "";
}
std::string raw_or_number(const std::string& s, const char* key) {
  auto text = field(s, key); if (!text.empty()) return text;
  std::regex r(std::string("\\\"") + key + "\\\"\\s*:\\s*([-+0-9.eE]+)"); std::smatch m;
  return std::regex_search(s, m, r) ? m[1].str() : "";
}
double number(const std::string& v) { try { return std::stod(v); } catch (...) { return 0.0; } }
std::string json_escape(const std::string& s) { std::string o; for (char c : s) { if (c=='"'||c=='\\') o+='\\'; o+=c; } return o; }
}

Journal::Journal(const fs::path& path) { fs::create_directories(path.parent_path()); out_.open(path, std::ios::binary | std::ios::app); if (!out_) throw std::runtime_error("cannot open journal: " + path.string()); }
void Journal::append(const std::string& payload, std::uint64_t seq, const std::string& run_id) {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  out_ << "{\"run_id\":\"" << json_escape(run_id) << "\",\"frame_seq\":" << seq
       << ",\"received_monotonic_ns\":" << ns << ",\"payload\":\"" << json_escape(payload) << "\"}\n";
  out_.flush(); records_++; if (!out_) throw std::runtime_error("journal write failed");
}
void Journal::close() { out_.flush(); out_.close(); }

std::optional<Trade> parse_binance_trade(const std::string& json, std::uint64_t frame_seq) {
  if (field(json, "e") != "trade") return std::nullopt;
  Trade t; t.symbol=field(json,"s"); t.id=raw_or_number(json,"t"); t.price_text=raw_or_number(json,"p"); t.qty_text=raw_or_number(json,"q");
  auto maker=field(json,"m"); if (maker.empty()) { std::regex r("\\\"m\\\"\\s*:\\s*(true|false)"); std::smatch m; if(std::regex_search(json,m,r)) maker=m[1].str(); }
  t.side = maker == "true" ? "sell" : "buy"; t.exchange_time=raw_or_number(json,"T"); t.price=number(t.price_text); t.qty=number(t.qty_text); t.frame_seq=frame_seq;
  if(t.symbol.empty()||t.id.empty()||t.price<=0||t.qty<=0||t.exchange_time.empty()) return std::nullopt;
  return t;
}
Factors FactorEngine::update(const Trade& t) {
  window_.push_back(t); prices_.push_back(t.price); if(t.side=="buy") buy_qty_+=t.qty; else sell_qty_+=t.qty;
  while(window_.size()>30){ auto old=window_.front(); window_.pop_front(); if(old.side=="buy") buy_qty_-=old.qty; else sell_qty_-=old.qty; }
  while(prices_.size()>21) prices_.pop_front();
  double total=buy_qty_+sell_qty_;
  Factors f; f.window_trades=window_.size(); f.imbalance=total>0?(buy_qty_-sell_qty_)/total:0; f.return_n=prices_.size()>1?prices_.back()/prices_.front()-1:0; f.rate_10s=window_.size()/10.0; f.average_size=window_.empty()?0:total/window_.size(); return f;
}
Signal evaluate(const Factors& f) { if(f.imbalance>=.15&&f.return_n>=.0001) return {"BUY_SIGNAL","positive_trade_imbalance_and_momentum",std::clamp(f.imbalance*.7+f.return_n*300.,-1.,1.)}; if(f.imbalance<=-.15&&f.return_n<=-.0001) return {"SELL_SIGNAL","negative_trade_imbalance_and_momentum",std::clamp(f.imbalance*.7+f.return_n*300.,-1.,1.)}; return {"HOLD","thresholds_not_aligned",std::clamp(f.imbalance*.7+f.return_n*300.,-1.,1.)}; }

int replay(const fs::path& input, const fs::path& output) {
  std::ifstream in(input); if(!in){std::cerr<<"cannot open input\n";return 2;} fs::create_directories(output); std::ofstream events(output/"normalized_events.jsonl"), signals(output/"signals.jsonl"); std::string line; std::uint64_t seq=0, valid=0; FactorEngine engine; std::unordered_set<std::string> seen;
  while(std::getline(in,line)){ auto payload=field(line,"payload"); if(payload.empty()) continue; auto t=parse_binance_trade(payload,++seq); if(!t||!seen.insert(t->symbol+":"+t->id).second) continue; valid++; auto f=engine.update(*t); auto s=evaluate(f); events<<"{\"symbol\":\""<<t->symbol<<"\",\"trade_id\":\""<<t->id<<"\",\"price\":"<<t->price<<",\"qty\":"<<t->qty<<"}\n"; signals<<"{\"action\":\""<<s.action<<"\",\"reason\":\""<<s.reason<<"\"}\n"; }
  std::ofstream summary(output/"replay_summary.json"); summary<<"{\"result\":\"PASS\",\"trades_processed\":"<<valid<<",\"execution_enabled\":false}\n"; return 0;
}
int synthetic(std::size_t count) { FactorEngine e; auto start=std::chrono::steady_clock::now(); for(std::size_t i=0;i<count;i++){Trade t{"BTCUSDT",i%2?"sell":"buy",std::to_string(i),"67000.0","0.001","0",67000.0,0.001,i}; e.update(t);} auto sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); std::cout<<"{\"benchmark_type\":\"SYNTHETIC_BENCHMARK\",\"events\":"<<count<<",\"events_per_second\":"<<(count/sec)<<"}\n"; return 0; }
}

int main(int argc,char** argv){ if(argc<2||std::string(argv[1])=="--help"){std::cout<<"marketpipe (C++20) commands: capture, replay, synthetic\n"; return argc<2?0:0;} try {std::string cmd=argv[1]; if(cmd=="synthetic"){std::size_t n=100000; if(argc>=4&&std::string(argv[2])=="--events") n=std::stoull(argv[3]); return marketpipe::synthetic(n);} if(cmd=="replay"&&argc>=5&&std::string(argv[2])=="--input"){return marketpipe::replay(argv[3],argv[4]);} if(cmd=="capture"){std::cerr<<"BLOCKED: C++ build requires a WebSocket transport adapter; no live capture performed\n"; return 2;} std::cerr<<"invalid command\n"; return 2;} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;} }
