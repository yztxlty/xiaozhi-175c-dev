"""运行真实 TLS Connect，保护接收线程的内存来源与失败清理。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_tls_receiver_uses_external_stack_and_cleans_failed_creation(tmp_path):
    source = (ROOT / 'components/78__esp-ml307/src/esp/esp_ssl.cc').read_text()
    connect = source[source.index('bool EspSsl::Connect('):source.index('void EspSsl::Disconnect()')]
    program = r'''
#include <cassert>
#include <string>
#include <cstddef>
using esp_err_t = int;
using esp_tls_error_handle_t = int;
struct esp_tls_t {};
struct esp_tls_cfg_t { void* crt_bundle_attach; int timeout_ms; };
void* esp_crt_bundle_attach = nullptr;
constexpr int ESP_OK=0, ESP_ERR_NO_MEM=0x101, pdPASS=1;
constexpr int MALLOC_CAP_SPIRAM=4, MALLOC_CAP_8BIT=8, MALLOC_CAP_INTERNAL=16;
constexpr int ESP_SSL_EVENT_RECEIVE_TASK_EXIT=1;
int caps=0, destroyed=0; bool allocation_ok=true;
esp_tls_t tls;
esp_tls_t* esp_tls_init(){return &tls;}
int esp_tls_conn_new_sync(const char*,size_t,int,esp_tls_cfg_t*,esp_tls_t*){return 1;}
int esp_tls_get_error_handle(esp_tls_t*,int*){return -1;}
int esp_tls_get_and_clear_last_error(int,int*,int*){return 0;}
void esp_tls_conn_destroy(esp_tls_t*){++destroyed;}
void xEventGroupClearBits(int,int){}
void xEventGroupSetBits(int,int){}
void vTaskDelete(void*){}
void vTaskDeleteWithCaps(void*){}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,int*){caps=0;return allocation_ok?pdPASS:0;}
int xTaskCreateWithCaps(void(*)(void*),const char*,int,void*,int,int*,int c){caps=c;return allocation_ok?pdPASS:0;}
#define ESP_LOGE(...) ((void)0)
class EspSsl { public:
 esp_tls_t* tls_client_=nullptr; int event_group_=1, receive_task_handle_=0, last_error_=0;
 bool connected_=false, external_stack_=false;
 bool Connect(const std::string&,int); void ReceiveTask(){}
};
'''
    program += connect + r'''
int main(){
 EspSsl voice; assert(voice.Connect("fixture.invalid",443));
 assert(caps == (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
 EspSsl ok; ok.external_stack_=true; assert(ok.Connect("fixture.invalid",443));
 assert(caps == (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 allocation_ok=false; EspSsl fail;
 assert(!fail.Connect("fixture.invalid",443));
 assert(!fail.connected_ && fail.tls_client_==nullptr);
 assert(fail.last_error_==ESP_ERR_NO_MEM && destroyed==1);
}
'''
    path = tmp_path / 'tls.cc'
    path.write_text(program)
    binary = tmp_path / 'tls'
    subprocess.run(['c++', '-std=c++17', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    assert 'vTaskDeleteWithCaps(NULL);' in connect
    network = (ROOT / 'components/78__esp-ml307/src/esp/esp_network.cc').read_text()
    assert 'std::make_unique<EspSsl>(connect_id == 2 || connect_id == 3)' in network
