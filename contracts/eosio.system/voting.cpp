/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include "eosio.system.hpp"

#include <eosiolib/eosio.hpp>
#include <eosiolib/crypto.h>
#include <eosiolib/print.hpp>
#include <eosiolib/datastream.hpp>
#include <eosiolib/serialize.hpp>
#include <eosiolib/multi_index.hpp>
#include <eosiolib/privileged.hpp>
#include <eosiolib/singleton.hpp>
#include <eosiolib/transaction.hpp>
#include <eosio.token/eosio.token.hpp>

#include <algorithm>
#include <cmath>

#define VOTE_VARIATION 0.1 

namespace eosiosystem {
   using eosio::indexed_by;
   using eosio::const_mem_fun;
   using eosio::bytes;
   using eosio::print;
   using eosio::singleton;
   using eosio::transaction;
   /**
    *  This method will create a producer_config and producer_info object for 'producer'
    *
    *  @pre producer is not already registered
    *  @pre producer to register is an account
    *  @pre authority of producer to register
    *
    */
   void system_contract::regproducer( const account_name producer, const eosio::public_key& producer_key, const std::string& url, uint16_t location ) {
      eosio_assert( url.size() < 512, "url too long" );
      eosio_assert( producer_key != eosio::public_key(), "public key should not be the default value" );
      require_auth( producer );

      auto prod = _producers.find( producer );

      if ( prod != _producers.end() ) {
         _producers.modify( prod, producer, [&]( producer_info& info ){
               info.producer_key = producer_key;
               info.is_active    = true;
               info.url          = url;
               info.location     = location;
            });
      } else {
         _producers.emplace( producer, [&]( producer_info& info ){
               info.owner         = producer;
               info.total_votes   = 0;
               info.producer_key  = producer_key;
               info.is_active     = true;
               info.url           = url;
               info.location      = location;
         });
      }
   }

   void system_contract::unregprod( const account_name producer ) {
      require_auth( producer );

      const auto& prod = _producers.get( producer, "producer not found" );

      _producers.modify( prod, 0, [&]( producer_info& info ){
            info.deactivate();
      });
   }

   void system_contract::update_elected_producers( block_timestamp block_time ) {
      _gstate.last_producer_schedule_update = block_time;

      auto idx = _producers.get_index<N(prototalvote)>();

      std::vector< std::pair<eosio::producer_key,uint16_t> > top_producers;
      top_producers.reserve(21);

      for ( auto it = idx.cbegin(); it != idx.cend() && top_producers.size() < 21 && 0 < it->total_votes && it->active(); ++it ) {
         top_producers.emplace_back( std::pair<eosio::producer_key,uint16_t>({{it->owner, it->producer_key}, it->location}) );
      }

      if ( top_producers.size() < _gstate.last_producer_schedule_size ) {
         return;
      }

      /// sort by producer name
      std::sort( top_producers.begin(), top_producers.end() );

      std::vector<eosio::producer_key> producers;

      producers.reserve(top_producers.size());
      for( const auto& item : top_producers )
         producers.push_back(item.first);

      bytes packed_schedule = pack(producers);

      if( set_proposed_producers( packed_schedule.data(),  packed_schedule.size() ) >= 0 ) {
         _gstate.last_producer_schedule_size = static_cast<decltype(_gstate.last_producer_schedule_size)>( top_producers.size() );
      }
   }
   
   /*
   * This function caculates the inverse weight voting. 
   * The maximum weight vote weight will be reached if a producer vote for the maximum producers registered.  
   */   
   double system_contract::inverseVoteWeight(int64_t staked, double amountVotedProducers, double variation) {
     if (amountVotedProducers == 0.0) {
       return 0;
     }

     double totalProducers = double(std::distance(_producers.begin(), _producers.end()));
     // 30 max producers allowed to vote
     if(totalProducers > 30) {
       totalProducers = 30;
     }

     double k = 1 - variation;
     
     return (k * sin(M_PI_2 * (amountVotedProducers / totalProducers)) + variation) * double(staked);
   }

   /**
    *  @pre producers must be sorted from lowest to highest and must be registered and active
    *  @pre if proxy is set then no producers can be voted for
    *  @pre if proxy is set then proxy account must exist and be registered as a proxy
    *  @pre every listed producer or proxy must have been previously registered
    *  @pre voter must authorize this action
    *  @pre voter must have previously staked some EOS for voting
    *  @pre voter->staked must be up to date
    *
    *  @post every producer previously voted for will have vote reduced by previous vote weight
    *  @post every producer newly voted for will have vote increased by new vote amount
    *  @post prior proxy will proxied_vote_weight decremented by previous vote weight
    *  @post new proxy will proxied_vote_weight incremented by new vote weight
    *
    *  If voting for a proxy, the producer votes will not change until the proxy updates their own vote.
    */
   void system_contract::voteproducer(const account_name voter_name, const account_name proxy, const std::vector<account_name> &producers) {
     require_auth(voter_name);
     update_votes(voter_name, proxy, producers, true);
   }
   
   void system_contract::checkNetworkActivation(){
     if( _gstate.total_activated_stake >= min_activated_stake && _gstate.thresh_activated_stake_time == 0 ) {
            _gstate.thresh_activated_stake_time = current_time();
    }
   } 

   void system_contract::update_votes( const account_name voter_name, const account_name proxy, const std::vector<account_name>& producers, bool voting ) {
      //validate input
      if ( proxy ) {
         eosio_assert( producers.size() == 0, "cannot vote for producers and proxy at same time" );
         eosio_assert( voter_name != proxy, "cannot proxy to self" );
         require_recipient( proxy );
      } else {
         eosio_assert( producers.size() <= 30, "attempt to vote for too many producers" );
         for( size_t i = 1; i < producers.size(); ++i ) {
            eosio_assert( producers[i-1] < producers[i], "producer votes must be unique and sorted" );
         }
      }

      auto voter = _voters.find(voter_name);
      eosio_assert( voter != _voters.end(), "user must stake before they can vote" ); /// staking creates voter object
      eosio_assert( !proxy || !voter->is_proxy, "account registered as a proxy is not allowed to use a proxy" );

      auto totalStaked = voter->staked;
      if(proxy){
         auto pxy = _voters.find(proxy);
         totalStaked += pxy->proxied_vote_weight;
      }
      auto inverse_stake = inverseVoteWeight(totalStaked, (double) producers.size(), VOTE_VARIATION);
      auto new_vote_weight = inverse_stake;
      
      /**
       * The first time someone votes we calculate and set last_vote_weight, since they cannot unstake until
       * after total_activated_stake hits threshold, we can use last_vote_weight to determine that this is
       * their first vote and should consider their stake activated.
       * 
       * Setting a proxy will change the global staked if the proxied producer has voted. 
       */
      if( voter->last_vote_weight <= 0.0 && producers.size() > 0 && voting ) {
          _gstate.total_activated_stake += voter->staked;
          
          if(voter->proxied_vote_weight > 0) {
           _gstate.total_activated_stake += voter->proxied_vote_weight;
          }

          checkNetworkActivation();
      } else if(voter->last_vote_weight <= 0.0 && proxy && voting) {
        auto prx = _voters.find(proxy);
        if(prx->last_vote_weight > 0){
          _gstate.total_activated_stake += voter->staked;
          checkNetworkActivation();
        }
      } else if(producers.size() == 0 && !proxy && voting ) {
         _gstate.total_activated_stake -= voter->staked;

         if(voter->proxied_vote_weight > 0) {
           _gstate.total_activated_stake -= voter->proxied_vote_weight;
         }
      }

      boost::container::flat_map<account_name, pair<double, bool /*new*/> > producer_deltas;

      //Voter from second vote
      if ( voter->last_vote_weight > 0 ) {
           
         //if voter account has set proxy to a other voter account
         if( voter->proxy ) { 
            auto old_proxy = _voters.find( voter->proxy );

            eosio_assert( old_proxy != _voters.end(), "old proxy not found" ); //data corruption
            _voters.modify( old_proxy, 0, [&]( auto& vp ) {
                  vp.proxied_vote_weight -= voter->last_vote_weight;
               });
            propagate_weight_change( *old_proxy );
         } 
         else {
            for( const auto& p : voter->producers ) {
               auto& d = producer_deltas[p];
               d.first -= voter->last_vote_weight;
               d.second = false;
            }
         }
      }

      if( proxy ) {
         auto new_proxy = _voters.find( proxy );
         eosio_assert( new_proxy != _voters.end(), "invalid proxy specified" ); //if ( !voting ) { data corruption } else { wrong vote }
         eosio_assert( !voting || new_proxy->is_proxy, "proxy not found" );
        
        if( voting ) {
         _voters.modify( new_proxy, 0, [&]( auto& vp ) {
              vp.proxied_vote_weight += voter->staked;
            });
         } else {
            _voters.modify( new_proxy, 0, [&]( auto& vp ) {
              vp.proxied_vote_weight = voter->staked;
          });
        }
         
        if((*new_proxy).last_vote_weight > 0){
            propagate_weight_change( *new_proxy );
        }
      } else {
         if( new_vote_weight >= 0 ) {
           //if voter is proxied
           //remove staked provided to account and propagate new vote weight
            if(voter->proxy){
             auto old_proxy = _voters.find( voter->proxy );
              eosio_assert( old_proxy != _voters.end(), "old proxy not found" ); //data corruption
              _voters.modify( old_proxy, 0, [&]( auto& vp ) {
                  vp.proxied_vote_weight -= voter->staked;
               });
              propagate_weight_change( *old_proxy );
            }
            if( voting ) {
              for( const auto& p : producers ) {
                auto& d = producer_deltas[p]; 
                d.first += new_vote_weight;
                d.second = true;
              }
            } else { //delegate bandwidth
              if(voter->last_vote_weight > 0){
                propagate_weight_change(*voter);
              }
            }
         }
      }

      for( const auto& pd : producer_deltas ) {
         auto pitr = _producers.find( pd.first );
         if( pitr != _producers.end() ) {
            eosio_assert( !voting || pitr->active() || !pd.second.second /* not from new set */, "producer is not currently registered" );
            _producers.modify( pitr, 0, [&]( auto& p ) {
               p.total_votes += pd.second.first;
               if ( p.total_votes < 0 ) { // floating point arithmetics can give small negative numbers
                  p.total_votes = 0;
               }
               _gstate.total_producer_vote_weight += pd.second.first;
               //eosio_assert( p.total_votes >= 0, "something bad happened" );
            });
         } else {
            eosio_assert( !pd.second.second /* not from new set */, "producer is not registered" ); //data corruption
         }
      }

      _voters.modify( voter, 0, [&]( auto& av ) {
         av.last_vote_weight = new_vote_weight;
         av.producers = producers;
         av.proxy     = proxy;
      });
   }

   /**
    *  An account marked as a proxy can vote with the weight of other accounts which
    *  have selected it as a proxy. Other accounts must refresh their voteproducer to
    *  update the proxy's weight.
    *
    *  @param isproxy - true if proxy wishes to vote on behalf of others, false otherwise
    *  @pre proxy must have something staked (existing row in voters table)
    *  @pre new state must be different than current state
    */
   void system_contract::regproxy( const account_name proxy, bool isproxy ) {
      require_auth( proxy );
      auto pitr = _voters.find(proxy);
      if ( pitr != _voters.end() ) {
         eosio_assert( isproxy != pitr->is_proxy, "action has no effect" );
         eosio_assert( !isproxy || !pitr->proxy, "account that uses a proxy is not allowed to become a proxy" );
         _voters.modify( pitr, 0, [&]( auto& p ) {
          p.is_proxy = isproxy;
        });    
      } else {
         _voters.emplace( proxy, [&]( auto& p ) {
            p.owner  = proxy;
            p.is_proxy = isproxy;
       });
      }
   }

   void system_contract::propagate_weight_change(const voter_info &voter) {
     eosio_assert( voter.proxy == 0 || !voter.is_proxy, "account registered as a proxy is not allowed to use a proxy");
     
     //getting all active producers
     auto totalProds = 0;
     for (const auto &prod : _producers) {
       if(prod.isActive()) { 
         totalProds++;
       }
     }
     auto totalStake = voter.staked + voter.proxied_vote_weight;
     double new_weight = inverseVoteWeight(totalStake, totalProds, VOTE_VARIATION);
    
     if (voter.proxy) {
       auto &proxy = _voters.get(voter.proxy, "proxy not found"); // data corruption
       _voters.modify(proxy, 0, [&](auto &p) { p.proxied_vote_weight = proxy.staked; });
       
       propagate_weight_change(proxy);
     } else {
       for (auto acnt : voter.producers) {
         auto &pitr = _producers.get(acnt, "producer not found"); // data corruption
         _producers.modify(pitr, 0, [&](auto &p) {
           p.total_votes = new_weight;
         });
       }
     }
     _voters.modify(voter, 0, [&](auto &v) { 
        v.last_vote_weight = new_weight; 
      });
   }

} /// namespace eosiosystem
