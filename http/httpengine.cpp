#include<httpengine.h>
#include<epool.h>
#include<vector>
using namespace my;

EngineRouter::EngineRouter(string url):prefix_url(url),reg_router(nullptr){

}

EngineRouter::~EngineRouter(){
    for(auto router:routers){
        delete router.second;
    }
    if(reg_router)
        delete reg_router;
}

bool EngineRouter::Get(std::string router,CallBackFunc func){
    return Register(HttpMethod::GET,router,func);
}

bool EngineRouter::Post(std::string router,CallBackFunc func){
    return Register(HttpMethod::POST,router,func);
}

bool EngineRouter::PUT(std::string router,CallBackFunc func){
    return Register(HttpMethod::PUT,router,func);
}

bool EngineRouter::Delete(std::string router,CallBackFunc func){
    return Register(HttpMethod::DELETE,router,func);
}

EngineRouter* EngineRouter::CreateRouter(std::string router){
    int idx = router.find_first_of('/',0);
    auto cur_url= router.substr(0,idx);
    auto ret = new EngineRouter(cur_url);
    this->routers[cur_url] = ret;
    return ret;
}

EngineRouter* EngineRouter::FindRouter(std::string router){
    if(router == "" ||!this) return this;
    int idx = router.find_first_of('/',0);
    auto cur_url= router.substr(0,idx);
    auto new_url = router.substr(idx + 1,router.size() - idx -1);
    // 直接从key里面取
    auto next_router = this->routers[cur_url];
    //{ID} 的情况下
    if(!next_router && cur_url[0] == '{' && cur_url.back() == '}'){
        if(!reg_router) reg_router = CreateRouter(router);
        next_router = reg_router;
    }
    // 未注册路由
    if(!next_router){
        next_router = CreateRouter(router);
    }
    return next_router->FindRouter(new_url);
}

std::string EngineRouter::FixRouter(std::string router){
    int start = 0;
    int end = router.size() - 1;
    while(router[start] =='/' || router[start] == '\\') start++;
    if(router[end] != '/' && router[end] != '\\'){
        end++;
        router.push_back('/');
    }
    for(int i = start;i<end;i++){
        if(router[i] == '\\') router[i] = '/';
    }
    return router.substr(start,end - start + 1);
}

bool EngineRouter::Register(HttpMethod_t method,std::string router,CallBackFunc func){
    if(router.size() <= 0) return false;
    router = FixRouter(router);
    auto routers = SpliteRouter(router);
    auto curr = this;
    for(auto router:routers){
        EngineRouter* next_router = nullptr;
        if(router[0] == '{' && router.back() == '}'){
            if(!reg_router){
                 reg_router = CreateRouter(router);
            }
            next_router = reg_router;
            if(router != next_router->prefix_url){
                return false;
            }
        }else{
            next_router = curr->routers[router];
        }
        if(!next_router){
            next_router = CreateRouter(router);
        }
        auto next_router = curr->routers[router];
    }
    if(curr == nullptr){
        return false;
    }
    if(curr->method_handler.count(method)){
        std::cerr<<"You cannot register multiple handlers for the same route."<<std::endl;
        return false;
    }
    curr->method_handler[method] = func;
}

/*
return: /url/xxx/
*/
std::string EngineRouter::VisiteURL(vector<CallBackFunc>& handlers,std::string router,HttpMethod_t method){
    if(router.size() <= 0) return ;
    bool noRouter = false;
    string path = this->prefix_url;
    EngineRouter* curr = this;
    router = FixRouter(router);
    auto routers = SpliteRouter(router);
    for(auto router:routers){
        for(auto handler:curr->handlerList){
            handlers.push_back(handler);
        }   
        if(curr->routers.count(router)){
            curr = curr->routers[router];
            continue;
        }
        if(curr->reg_router){
            curr = curr->reg_router;
            continue;
        }
        noRouter = true;
        path +=  curr->prefix_url  + "/";
    }
    if(!noRouter && curr->method_handler.count(method)){
        for(auto handler:curr->handlerList){
            handlers.push_back(handler);
        }
        handlers.push_back(curr->method_handler[method]);
        return path;
    }
    handlers = vector<CallBackFunc>();
    return "";
}

int EngineRouter::StaticFile(std::string router,std::string path){
    return 0;
}



class ClientFunc:public HandleEventOBJ{
    Epool* ep;
    HttpEngine* server;
public:
    ClientFunc(Epool* ep,HttpEngine* server):ep(ep),server(server){}
    void handle(epoll_event event){
        if(event.events & EPOLLIN){
            auto conn = server->conn[event.data.fd];
            if(!conn) return;
            int ret = conn->ReadToRequest();
            if(ret < 0){
                perror("ReadString");
                return ;
            }
            if(!ret){
                ep->DelEvent(conn);
                conn->ShutDown(SHUT_RDWR);
                conn->Close();
                return ;
            }
            vector<CallBackFunc> handlers;
            conn->URL = server->VisiteURL(handlers,conn->Router(),conn->Method());
            // 404 的情况
            if(conn->URL == ""){
                if(server->NoRouterFunc) server->NoRouterFunc(conn);  
            }
            // 正常情况 先调用钩子函数 最后调用处理函数
            for(auto handler:handlers){
                handler(conn);
            }
            ep->ModifyEvent(conn,EPOLLIN|EPOLLRDHUP|EPOLLET|EPOLLONESHOT);
        }
        if(event.events & (EPOLLRDHUP)){
            auto conn = server->conn[event.data.fd];
            ep->DelEvent(conn);
            conn->ShutDown(SHUT_RDWR);
            conn->Close();
            return ;
        }
        return ;
    }
};

class ServerFunc:public HandleEventOBJ{
    Epool* ep;
    HttpEngine* server;
    public:
        ServerFunc(Epool* ep,HttpEngine* server):ep(ep),server(server){}
    public:
        void handle(epoll_event event){
            auto socket = server->Accept();
            if(server->max_conn  <= server->current_conn){
                // 执行拒绝连接函数
                socket.ShutDown(SHUT_RDWR);
                socket.Close();
                return ;
            }
            server->AddConn(socket);
            ep->AddEvenet(&socket,EPOLLIN|EPOLLRDHUP|EPOLLET|EPOLLONESHOT,shared_ptr<HandleEventOBJ>(new ClientFunc(ep,server)));
        }
};




HttpEngine::HttpEngine(int port,int max_conn):TCPServer("0.0.0.0",port),\
NoRouterFunc(nullptr),_mode(HttpMode::M_Default),max_conn(max_conn),current_conn(0),live(true),\
EngineRouter("/"){}


HttpEngine::~HttpEngine(){
   EngineRouter::~EngineRouter();
    for(auto con:conn){
        con.second->ShutDown(SHUT_RDWR);
        con.second->Close();
        delete con.second;
    }
}

void HttpEngine::SetMode(HttpMode_t _http_mode){
    this->_mode = _http_mode;
}

CallBackFunc HttpEngine::NoRouter(CallBackFunc func){
    auto ret = NoRouterFunc;
    NoRouterFunc = func;
    return ret;
}

void HttpEngine::Run(){
    if(Listen()<0){
        perror("Listen failed");
        return ;
    }
    my::Epool ep;
    ep.EventInit();
    auto func = shared_ptr<HandleEventOBJ>(new ServerFunc(&ep,this));
    ep.AddEvenet(this,EPOLLIN|EPOLLET,func);
    ep.LoopWait();
}

void HttpEngine::AddConn(TCPSocket socket){

}

int HttpEngine::VisiteAllRouter(EngineRouter* curr,string url,vector<RouterNode>& routerList){
    // 放入所有的叶子节点
    for(auto handler:method_handler){
        routerList.push_back(RouterNode{
            url:url,
            method:handler.first,
        });
    }
    if(this->reg_router) {
        routerList.push_back(RouterNode{
        url:url,
        method:"",
        });
    }

    for(auto router:routers){
        auto newurl =  url + router.first + "/";
        VisiteAllRouter(router.second,newurl,routerList);
    }
    return 0;
}

int HttpEngine::GetAllRouter(vector<RouterNode>& routerList){
    EngineRouter* router = (EngineRouter*)this;
    string url = this->prefix_url;
    return VisiteAllRouter(router,url,routerList);
}