// the caching lock server implementation

#include "lock_server_cache_rsm.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"


static void *
outgoingthread(void *x)
{
  lock_server_cache_rsm *sc = (lock_server_cache_rsm *) x;
  sc->outgoing();
  return 0;
}

lock_server_cache_rsm::lock_server_cache_rsm(class rsm *_rsm) 
  : rsm (_rsm)
{
  pthread_t th;
  int r = pthread_create(&th, NULL, &outgoingthread, (void *) this);
  VERIFY (r == 0);
  //init mutex
  pthread_mutex_init(&mu, NULL);
  rsm->set_state_transfer(this);
}

void
lock_server_cache_rsm::outgoing()
{

  // This method should be a continuous loop, that sends revoke
  // messages to lock holders whenever another client wants the
  // same lock
  //just handles the thread safe blocking rpc queue and makes all of the rpc calls
  tprintf("\nstarted outgoing thread.\n");
  while(1){
    rpc_call rp = rpc_queue.dequeue();
    //if not primary dont make rpc call
    if(!rsm->amiprimary()){
      tprintf("\nnot primary so wont make rpc call to client\n");
      continue;
    }
    
    int ret;
    tprintf("\nI am the primary, so making rpc call\n");
    if(rp.rpc == lock_server_cache_rsm::REVOKE){
        tprintf("\nmaking revoke rpc call for lock %llu\n",rp.lock_id);
        int r;
        ret = revoke_helper(rp.lock_id,rp.name,rp.xid,r);

    }else if(rp.rpc == lock_server_cache_rsm::RETRY){
         tprintf("\nmaking retry rpc call for lock %llu\n",rp.lock_id);
         ret = retry_helper(rp.lock_id,rp.name,false,rp.xid);
    }else{
        //theres a waiting line for this lock
        tprintf("\nmaking retry with wait rpc call for lock %llu\n",rp.lock_id);
        ret = retry_helper(rp.lock_id,rp.name,true,rp.xid);
    }

    if(ret!=rlock_protocol::OK){
        tprintf("rpc call did not return OK");
    }

  }

}




int lock_server_cache_rsm::acquire(lock_protocol::lockid_t lid, std::string id, 
             lock_protocol::xid_t xid, int &)
{
  ScopedLock sl(&mu);
  tprintf("\nacquiring lock on server for client %s with xid %llu\n",id.c_str(),xid);
  nacquire++;

  lock_entry le = lock_map[lid];
  
  

  if(le.local_state==FREE){
    //le.queue should be empty...
    tprintf("\nlock was free, queueing retry call\n");
    lock_map[lid].owner = id;
    lock_map[lid].local_state = LOCKED;
    lock_map[lid].xid = xid;
    //queue retry call
    rpc_call call;
    call.rpc = lock_server_cache_rsm::RETRY;
    call.lock_id=lid;
    call.name = id;
    call.xid = xid;
    rpc_queue.enqueue(call);


  }

  //ignore an acquire by the owner
  else if(le.owner == id){
    tprintf("\nrecieved an acquire by the owner... ignoring it\n");
  }

  else if(le.local_state == LOCKED){
    //le.queue should be empty
    tprintf("\nlock is taken by %s, queueing revoke call with xid %llu\n",le.owner.c_str(),le.xid);
    lock_map[lid].waiting.push_back(id);
    lock_map[lid].local_state = ACQ;
    lock_map[lid].xid_map[id] = xid;
    //queue revoke call
    rpc_call call;
    call.rpc = lock_server_cache_rsm::REVOKE;
    call.name = le.owner;
    call.lock_id = lid;
    call.xid = le.xid;
    rpc_queue.enqueue(call);

  }

  else if(le.local_state == ACQ){
    //le.queue should be non-empty
    tprintf("\nserver is already attempting to acquire lock, queueing to waiting list\n");
    //check if this is a duplicate acquire
    std::deque<std::string>::iterator it;
    
    for(it = le.waiting.begin();it<le.waiting.end();it++){
     //find this id if it exists and update the xid       
     std::string pend = *it;
     if(pend == id){
       tprintf("\nhad a duplicate acquire call\n");
       lock_map[lid].xid_map[id] = xid;
       break;
     }     
      
    }
    //if the id didnt exist push it back
    if(it == le.waiting.end()){
      lock_map[lid].waiting.push_back(id);
      lock_map[lid].xid_map[id] = xid;
     }
    //send another revoke
    rpc_call call;
    call.rpc = lock_server_cache_rsm::REVOKE;
    call.name = le.owner;
    call.lock_id = lid;
    call.xid = le.xid;
    rpc_queue.enqueue(call);


  }else{
    //shouldnt get here
    tprintf("\nacquire got to an unexpected state----------------------\n");
  }

  //return RETRY so client will wait for a queued rpc retry call
  return lock_protocol::RETRY;


}

int 
lock_server_cache_rsm::release(lock_protocol::lockid_t lid, std::string id, 
         lock_protocol::xid_t xid, int &r)
{
  ScopedLock sl(&mu);
  tprintf("\nreleasing lock %llu on server from %s with xid %llu\n",lid,id.c_str(),xid);
  lock_protocol::status ret = lock_protocol::OK;

  //ignore duplicate releases (if either owner or xid is wrong)
  if(lock_map[lid].owner != id || lock_map[lid].xid != xid)
    return ret;

  nacquire--;
  if(lock_map[lid].waiting.empty()){
    //state should be LOCKED
    tprintf("\nno wait on the lock, freeing it\n");
    lock_map[lid].local_state = FREE;
    //clear owner
    lock_map[lid].owner = "";
  }else{
    //queue is not empty, send lock to next in line
    std::string next = lock_map[lid].waiting.front();
    tprintf("\nsending lock to next in line: %s\n",next.c_str());
    lock_map[lid].waiting.pop_front();
    lock_map[lid].owner = next;
    //change xid and put it in rpc call
    lock_map[lid].xid = lock_map[lid].xid_map[next];
    lock_map[lid].xid_map.erase(next);

    rpc_call call;
    call.name = next;
    call.lock_id = lid;
    call.xid = lock_map[lid].xid;
    //see if there is anyone else waiting
    if(lock_map[lid].waiting.empty()){
      tprintf("\nno more wait after %s\n",next.c_str());
      lock_map[lid].local_state = LOCKED;
      call.rpc = lock_server_cache_rsm::RETRY;
    }else{
       tprintf("\nstill a wait so need lock back\n");
       //state should stay acquiring
       call.rpc = lock_server_cache_rsm::RETRY_WAIT;
    }
    rpc_queue.enqueue(call);
  }

  return ret;
}

std::string
lock_server_cache_rsm::marshal_state()
{
  ScopedLock sl(&mu);
  tprintf("\nMarshalling state for lock server\n");
  marshall rep;
  //marshall lock_map
  
  //start with map size
  rep << lock_map.size();
  //iterate through map
  map<lock_protocol::lockid_t,lock_entry>::iterator it;
  for(it = lock_map.begin();it!=lock_map.end(); it++){
    //get lid/lock_entry pair
    lock_protocol::lockid_t lid = it->first;
    lock_entry le = lock_map[lid];
    rep << lid;
    //input xid, local_state, and owner
    rep << le.xid;
    rep << le.local_state;
    rep << le.owner;
    //serialize queue
    rep << le.waiting.size();
    std::deque<std::string>::iterator qit;
    for(qit = le.waiting.begin();qit!=le.waiting.end();qit++){
      rep << *qit;
 
    }    

    //serialize xid_map
    std::map<std::string,lock_protocol::xid_t>::iterator xit;
    rep << le.xid_map.size();
    for(xit = le.xid_map.begin();xit!=le.xid_map.end();xit++){
      std::string wait = xit->first;
      rep << wait;
      rep << le.xid_map[wait]; 


    }

  }

  return rep.str();
}

void
lock_server_cache_rsm::unmarshal_state(std::string state)
{
  ScopedLock sl(&mu);
  //unmarshall lock_map
  unmarshall rep(state);
  //first clear current map
  lock_map.clear();
  //then fill it back up
  unsigned int map_size;
  rep >> map_size;
  
  unsigned int i;
  for(i=0;i<map_size;i++){
    //get lid
    unsigned long long lid;
    rep >> lid;
    lock_entry le;
    //get xid, local_state, and owner
    unsigned long long xid;
    int local_state;
    rep >> xid;
    le.xid = xid;
    rep >> local_state;
    //cant figure out enum casting so doing it the hard way
    if(local_state == LOCKED)
      le.local_state = LOCKED;
    else if(local_state == FREE)
      le.local_state = FREE;
    else if(local_state == ACQ)
      le.local_state = ACQ;
    else if(local_state == REL)
      le.local_state = REL;
    else 
      VERIFY(0); //we have a bad state

    rep >> le.owner;
    //deserialize waiting queue
    unsigned int deque_size;
    rep >> deque_size;
    unsigned int j;
    
    for(j=0;j<deque_size;j++){
      std::string wait;
      rep >> wait;
      le.waiting.push_back(wait);

    }

    unsigned int xid_size;
    rep >> xid_size;
    unsigned int k;
    for(k=0;k<xid_size;k++){
      std::string wait;
      rep >> wait;
      unsigned long long wait_xid;
      rep >> wait_xid;
      le.xid_map[wait] = wait_xid;

    }

    //lock_entry should be done, now put it in map
    lock_map[lid] = le;


  }


}

lock_protocol::status
lock_server_cache_rsm::stat(lock_protocol::lockid_t lid, int &r)
{
  printf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}

int
lock_server_cache_rsm::retry_helper(lock_protocol::lockid_t lid,std::string id,bool wait,lock_protocol::xid_t xid){

   int ret = rlock_protocol::OK;
   tprintf("\nmaking a retry rpc call to %s with xid %llu\n",id.c_str(),xid);
   if(wait) {
     tprintf("\nThere is a wait on the server\n");        
   }
   else {
     tprintf("\nthere is no wait on the server\n");        
   }
   
   handle h(id);
   rpcc *cl = h.safebind();
   if(cl){
     int r;
     ret = cl->call(rlock_protocol::retry,lid,xid,wait,r);
   } else {
     tprintf("\nretry helper failed!\n");
   }
   return ret;
}

int
lock_server_cache_rsm::revoke_helper(lock_protocol::lockid_t lid,std::string id,lock_protocol::xid_t xid,int &r){

   int ret = rlock_protocol::OK;
   tprintf("\nmaking a revoke rpc call to %s with xid %llu\n",id.c_str(),xid);

   handle h(id);
   rpcc *cl = h.safebind();
   if(cl){
     ret = cl->call(rlock_protocol::revoke,lid,xid,r);
   } else {
     tprintf("\nrevoke helper failed!\n");
   }
   return ret;
}
