"""运行真实播放器/解封器；仅替换 FreeRTOS、解码库与扬声器硬件边界。"""
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

RUNTIME = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr int pdTRUE=1, pdFALSE=0, pdPASS=1;
constexpr TickType_t portMAX_DELAY=0xffffffff;
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
inline void* heap_caps_malloc(size_t n, int) {return std::malloc(n);}
inline void heap_caps_free(void* p) {std::free(p);}
struct Queue {std::mutex m; std::condition_variable cv; size_t capacity, width; std::deque<std::vector<uint8_t>> items;};
using QueueHandle_t=Queue*;
inline QueueHandle_t xQueueCreate(size_t n,size_t w) {auto q=new Queue; q->capacity=n;q->width=w;return q;}
inline bool wait_for(std::unique_lock<std::mutex>& l,std::condition_variable& cv,TickType_t t,std::function<bool()> p) {
 if(t==portMAX_DELAY){cv.wait(l,p);return true;} return cv.wait_for(l,std::chrono::milliseconds(t),p);
}
inline int xQueueSend(Queue* q,const void* p,TickType_t t){std::unique_lock l(q->m);if(!wait_for(l,q->cv,t,[&]{return q->items.size()<q->capacity;}))return 0;
 q->items.emplace_back((const uint8_t*)p,(const uint8_t*)p+q->width);q->cv.notify_all();return 1;}
inline int xQueueReceive(Queue* q,void* p,TickType_t t){std::unique_lock l(q->m);if(!wait_for(l,q->cv,t,[&]{return !q->items.empty();}))return 0;
 std::memcpy(p,q->items.front().data(),q->width);q->items.pop_front();q->cv.notify_all();return 1;}
inline int xQueueOverwrite(Queue* q,const void* p){std::lock_guard l(q->m);q->items.clear();q->items.emplace_back((const uint8_t*)p,(const uint8_t*)p+q->width);q->cv.notify_all();return 1;}
inline unsigned uxQueueMessagesWaiting(Queue* q){std::lock_guard l(q->m);return q->items.size();}
inline void vQueueDelete(Queue* q){delete q;}
using TaskHandle_t=std::thread*;
inline int xTaskCreateWithCaps(void(*f)(void*),const char*,size_t,void* p,int,TaskHandle_t* out,int){*out=new std::thread([=]{f(p);});return 1;}
inline void vTaskDeleteWithCaps(void*){}
inline void vTaskDelay(TickType_t t){std::this_thread::sleep_for(std::chrono::milliseconds(t));}
inline TickType_t xTaskGetTickCount(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Ring {std::mutex m;std::condition_variable cv;size_t cap,used=0;struct Item{void* p;size_t n;bool ready;};std::deque<Item> items;std::unordered_map<void*,size_t> acquired;};
using RingbufHandle_t=Ring*;
constexpr int RINGBUF_TYPE_NOSPLIT=0;
inline Ring* xRingbufferCreateWithCaps(size_t n,int,int){auto r=new Ring;r->cap=n;return r;}
inline int xRingbufferSendAcquire(Ring* r,void** p,size_t n,TickType_t t){std::unique_lock l(r->m);if(!wait_for(l,r->cv,t,[&]{return r->used+n+8<=r->cap;}))return 0;
 *p=std::malloc(n);r->used+=n+8;r->items.push_back({*p,n,false});return 1;}
inline int xRingbufferSendComplete(Ring* r,void* p){std::lock_guard l(r->m);for(auto& i:r->items)if(i.p==p)i.ready=true;r->cv.notify_all();return 1;}
inline void* xRingbufferReceive(Ring* r,size_t* n,TickType_t t){std::unique_lock l(r->m);if(!wait_for(l,r->cv,t,[&]{return !r->items.empty()&&r->items.front().ready;}))return nullptr;
 auto i=r->items.front();r->items.pop_front();*n=i.n;r->acquired[i.p]=i.n;return i.p;}
inline void vRingbufferReturnItem(Ring* r,void* p){std::lock_guard l(r->m);
 r->used-=r->acquired.at(p)+8;r->acquired.erase(p);std::free(p);r->cv.notify_all();}
inline void vRingbufferDeleteWithCaps(Ring* r){delete r;}
inline std::atomic_int decoded{0}, written{0}, device_state{0};
inline std::atomic_bool hold_decode{false}, decode_held{false}, fail_decode{false};
inline std::mutex samples_mutex;
inline std::vector<int> samples;
inline std::vector<std::chrono::steady_clock::time_point> output_times;
constexpr int kDeviceStateIdle=0;
struct Application{static Application& GetInstance(){static Application a;return a;}int GetDeviceState(){return device_state;}};
struct Codec{bool output_enabled(){return true;}void EnableOutput(bool){}int output_sample_rate(){return 24000;}
 void OutputData(std::vector<int16_t>& pcm){ {std::lock_guard l(samples_mutex);samples.push_back(pcm[0]);output_times.push_back(std::chrono::steady_clock::now());}++written;std::this_thread::sleep_for(std::chrono::microseconds(pcm.size()*1000000/24000));}};
struct Board{static Board& GetInstance(){static Board b;return b;}Codec* GetAudioCodec(){static Codec c;return &c;}};
using esp_opus_dec_frame_duration_t=int;
constexpr int ESP_OPUS_DEC_FRAME_DURATION_5_MS=5,ESP_OPUS_DEC_FRAME_DURATION_10_MS=10,ESP_OPUS_DEC_FRAME_DURATION_20_MS=20,ESP_OPUS_DEC_FRAME_DURATION_40_MS=40,ESP_OPUS_DEC_FRAME_DURATION_60_MS=60;
constexpr int ESP_AUDIO_MONO=1,ESP_AUDIO_ERR_OK=0,ESP_AUDIO_DEC_RECOVERY_NONE=0;
struct esp_opus_dec_cfg_t{uint32_t sample_rate;int channel,frame_duration;bool self_delimited;};
struct esp_audio_dec_in_raw_t{uint8_t* buffer;uint32_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t{uint8_t* buffer;uint32_t len,decoded_size;};
struct esp_audio_dec_info_t{};
inline int esp_opus_dec_open(esp_opus_dec_cfg_t* c,size_t,void** d){*d=new esp_opus_dec_cfg_t(*c);return 0;}
inline void esp_opus_dec_close(void* d){delete static_cast<esp_opus_dec_cfg_t*>(d);}
inline int esp_opus_dec_decode(void* d,esp_audio_dec_in_raw_t* in,esp_audio_dec_out_frame_t* out,esp_audio_dec_info_t*){
 if(fail_decode.exchange(false))return -1;
 if(hold_decode.exchange(false)){decode_held=true;while(decode_held)std::this_thread::sleep_for(1ms);}
 auto c=static_cast<esp_opus_dec_cfg_t*>(d);out->decoded_size=c->sample_rate*c->frame_duration/1000*2;assert(out->len>=out->decoded_size);
 auto p=reinterpret_cast<int16_t*>(out->buffer);std::fill(p,p+out->decoded_size/2,in->buffer[0]);++decoded;return 0;}
'''

CHECK = r'''
#include "runtime.h"
#include "music/music_player.h"
#include <iostream>
std::vector<uint8_t> page(const std::vector<std::vector<uint8_t>>& packets){std::vector<uint8_t> p(27,0);std::memcpy(p.data(),"OggS",4);p[26]=packets.size();
 for(auto& a:packets)p.push_back(a.size());for(auto& a:packets)p.insert(p.end(),a.begin(),a.end());return p;}
std::vector<uint8_t> stream(int n,uint8_t value){std::vector<uint8_t> head(19,0);std::memcpy(head.data(),"OpusHead",8);head[12]=0xc0;head[13]=0x5d;
 auto p=page({head,{'O','p','u','s','T','a','g','s'}});auto audio=page(std::vector<std::vector<uint8_t>>(n,{value}));p.insert(p.end(),audio.begin(),audio.end());return p;}
MusicPlayer* current_player=nullptr;
void until(std::function<bool()> p){auto limit=std::chrono::steady_clock::now()+7s;while(!p()){current_player->DispatchState();assert(std::chrono::steady_clock::now()<limit);std::this_thread::sleep_for(1ms);}}
int main(int argc,char** argv){std::string mode=argv[1];auto player=new MusicPlayer;std::atomic_bool completed=false,failed=false;
 current_player=player;
 player->OnState([&](uint32_t epoch,const char* s){if(std::strcmp(s,"COMPLETED")==0)completed=true;if(std::strcmp(s,"FAILED")==0)failed=true;});
 player->Begin(1,20);auto data=stream(mode=="tail"?3:(mode=="bounded"?240:120),7);
 if(mode=="seek")hold_decode=true;
 assert(player->Feed(data.data(),data.size()));
 if(mode!="jitter" && mode!="failure")assert(player->FeedEnd());
 if(mode=="seek"){
   until([]{return decode_held.load();});player->Begin(2,20);auto next=stream(4,9);assert(player->Feed(next.data(),next.size()));assert(player->FeedEnd());decode_held=false;
   until([&]{return completed.load();});std::lock_guard l(samples_mutex);assert(samples.size()==4);for(auto v:samples)assert(v==9);
 }else if(mode=="pause"){
   until([]{return written.load()>=1;});player->Pause();auto before=written.load();std::this_thread::sleep_for(100ms);assert(written.load()<=before+1);
   player->Resume();until([&]{return completed.load();});assert(written==120);
 }else if(mode=="prefetch"){
   until([]{return written.load()>=1;});assert(decoded.load()-written.load()>=40);until([&]{return completed.load();});assert(written==120);
 }else if(mode=="tail"){
   until([&]{return completed.load();});assert(written==3);
 }else if(mode=="jitter"){
   until([]{return written.load()>=1;});auto before=written.load();
   // 已缓冲后，模拟云端 600ms 没有新数据；输出仍应连续。
   std::this_thread::sleep_for(600ms);assert(written.load()>=before+20);
   auto next=page(std::vector<std::vector<uint8_t>>(120,{7}));assert(player->Feed(next.data(),next.size()));assert(player->FeedEnd());
   until([&]{return completed.load();});assert(written==240);
   std::lock_guard l(samples_mutex);for(size_t i=1;i<output_times.size();++i)assert(output_times[i]-output_times[i-1]<80ms);
 }else if(mode=="bounded"){
   until([]{return written.load()>=1;});player->Pause();std::this_thread::sleep_for(100ms);
   assert(decoded.load()-written.load()<=160);assert(decoded.load()<240);
   player->Resume();until([&]{return completed.load();});assert(written==240);
 }else if(mode=="failure"){
   until([]{return written.load()>=1;});fail_decode=true;auto next=page({{7}});assert(player->Feed(next.data(),next.size()));
   until([&]{return failed.load();});auto stopped=written.load();std::this_thread::sleep_for(100ms);assert(written.load()<=stopped+1);
 }
 std::cout<<mode<<" decoded="<<decoded<<" played="<<written<<std::endl;
 delete player;
 std::_Exit(0);
}
'''


@pytest.fixture(scope='module')
def playback_binary():
    directory = Path(tempfile.mkdtemp(prefix='ygsoul-music-test-'))
    (directory / 'freertos').mkdir()
    (directory / 'runtime.h').write_text(RUNTIME, encoding='utf-8')
    for name in ('freertos/FreeRTOS.h', 'freertos/queue.h', 'freertos/task.h',
                 'freertos/ringbuf.h', 'esp_audio_dec.h', 'esp_heap_caps.h',
                 'esp_log.h', 'esp_opus_dec.h', 'application.h', 'board.h'):
        (directory / name).write_text('#include "runtime.h"\n', encoding='utf-8')
    (directory / 'check.cc').write_text(CHECK, encoding='utf-8')
    binary = directory / 'check'
    subprocess.run(['c++', '-std=c++17', '-pthread', '-I', str(directory),
                    '-I', str(ROOT / 'main'), str(directory / 'check.cc'),
                    str(ROOT / 'main/music/music_player.cc'),
                    str(ROOT / 'main/audio/demuxer/ogg_demuxer.cc'),
                    '-o', str(binary)], check=True)
    return binary


@pytest.mark.parametrize('mode', ['pause', 'seek', 'prefetch', 'tail', 'jitter', 'bounded', 'failure'])
def test_music_runtime(playback_binary, mode):
    subprocess.run([str(playback_binary), mode], check=True, timeout=8)


def test_fast_seek_reply_cannot_be_cancelled_after_send(tmp_path):
    source = (ROOT / 'main/music/music_client.cc').read_text(encoding='utf-8')
    method = source[source.index('bool MusicClient::Seek('):source.index('bool MusicClient::StopPlayback(')]
    program = r'''
#include <atomic>
#include <cassert>
#include <string>
enum class MusicUiState {kPlaying,kPaused,kBuffering,kReady,kFailed};
struct MusicClient {
 std::atomic<MusicUiState> ui_state_{MusicUiState::kPlaying};
 std::atomic_uint32_t stream_epoch_{1};
 struct Player {bool active=true;void Stop(){active=false;}} player_;
 bool Seek(uint32_t);
 void SetUiState(MusicUiState s){ui_state_=s;}
 bool Send(const std::string&){stream_epoch_=2;player_.active=true;SetUiState(MusicUiState::kPlaying);return true;}
};
''' + method + r'''
int main(){MusicClient client;assert(client.Seek(30000));assert(client.stream_epoch_==2);
 assert(client.player_.active);assert(client.ui_state_==MusicUiState::kPlaying);}
'''
    cpp = tmp_path / 'seek.cc'
    cpp.write_text(program, encoding='utf-8')
    binary = tmp_path / 'seek'
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
