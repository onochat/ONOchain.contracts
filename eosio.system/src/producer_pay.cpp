#include <eosio.system/eosio.system.hpp>

#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

    const uint32_t daily_rate = 136986300000;          // 5B annual rate increase
    const uint32_t seconds_per_year = 365 * 24 * 3600; 	//  365 is more accurate than 52*7. Does not account for leap year.
    const uint32_t blocks_per_year = seconds_per_year * 2 ;   // half seconds per year
    const uint32_t blocks_per_hour = 2 * 3600;
    const uint32_t blocks_per_day = blocks_per_hour * 24 ;
    const int64_t useconds_per_day = 24 * 3600 * int64_t(1000000);
    const int64_t useconds_per_year = seconds_per_year * 1000000ll;

    void system_contract::onblock(ignore <block_header>) {
        using namespace eosio;

        require_auth(_self);

        block_timestamp timestamp;
        name producer;
        _ds >> timestamp >> producer;

        if (_gstate.last_pervote_bucket_fill == time_point())  /// start the presses
            _gstate.last_pervote_bucket_fill = current_time_point();


        /**
         * At startup the initial producer may not be one that is registered / elected
         * and therefore there may be no producer object for them.
         */
        auto prod = _producers.find(producer.value);
        if (prod != _producers.end()) {
            _gstate.total_unpaid_blocks++;
            _producers.modify(prod, same_payer, [&](auto &p) {
                p.unpaid_blocks++;
            });
        }

        /// only update block producers once every minute, block_timestamp is in half seconds
        if (timestamp.slot - _gstate.last_producer_schedule_update.slot > 120) {
            update_elected_producers(timestamp);

            if ((timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day) {
                name_bid_table bids(_self, _self.value);
                auto idx = bids.get_index<"highbid"_n>();
                auto highest = idx.lower_bound(std::numeric_limits<uint64_t>::max() / 2);
                if (highest != idx.end() &&
                    highest->high_bid > 0 &&
                    (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                    _gstate.thresh_activated_stake_time > time_point() &&
                    (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
                        ) {
                    _gstate.last_name_close = timestamp;
                    idx.modify(highest, same_payer, [&](auto &b) {
                        b.high_bid = -b.high_bid;
                    });
                }
            }
        }
    }

    using namespace eosio;

    void system_contract::claimrewards(const name owner) {
        require_auth(owner);

        const auto &prod = _producers.get(owner.value);
        eosio_assert(prod.active(), "producer does not have an active key");

        const auto ct = current_time_point();

        eosio_assert(ct - prod.last_claim_time > microseconds(useconds_per_day),
                     "already claimed rewards within past day");

//        const asset token_supply = eosio::token::get_supply(token_account, core_symbol().code());
        const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();

        if (usecs_since_last_fill > 0 && _gstate.last_pervote_bucket_fill > time_point()) {
            auto new_tokens = static_cast<int64_t>((daily_rate * double(usecs_since_last_fill)) /
                                                   double(useconds_per_day));

            // 20% to producer pay
            auto to_producers = (new_tokens * 20) / 100;

            // 65% to social media content
            auto to_social = (new_tokens * 65) / 100;

            // 5% OP behaviour
            auto to_op_fund = (new_tokens * 5) / 100;

            // 10% user behaviour
            auto to_user_fund = (new_tokens * 10) / 100;


            INLINE_ACTION_SENDER(eosio::token, issue)(
                    token_account,
                    {{_self, active_permission}},
                    {
                            _self,
                            asset(new_tokens, core_symbol()),
                            std::string("issue new tokens")
                    }
            );

            INLINE_ACTION_SENDER(eosio::token, transfer)(
                    token_account,
                    {{_self, active_permission}},
                    {
                            _self,
                            social_account,
                            asset(to_social, core_symbol()),
                            std::string("social media fund")
                    }
            );

            INLINE_ACTION_SENDER(eosio::token, transfer)(
                    token_account,
                    {{_self, active_permission}},
                    {
                            _self,
                            opfund_account,
                            asset(to_op_fund, core_symbol()),
                            std::string("op fund")
                    }
            );

            INLINE_ACTION_SENDER(eosio::token, transfer)(
                    token_account,
                    {{_self, active_permission}},
                    {
                            _self,
                            usfund_account,
                            asset(to_user_fund, core_symbol()),
                            std::string("user fund")
                    }
            );

            INLINE_ACTION_SENDER(eosio::token, transfer)(
                    token_account,
                    {{_self, active_permission}},
                    {
                            _self,
                            bpay_account,
                            asset(to_producers, core_symbol()),
                            std::string("fund per-block bucket")
                    }
            );

            _gstate.perblock_bucket += to_producers;
            _gstate.last_pervote_bucket_fill = ct;
        }

        auto prod2 = _producers2.find(owner.value);

        /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
        /// time duration the vote weight has been held into one metric.
//        const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);
//
//        bool crossed_threshold = (last_claim_plus_3days <= ct);
//        bool updated_after_threshold = true;
//        if (prod2 != _producers2.end()) {
//            updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
//        } else {
//            prod2 = _producers2.emplace(owner, [&](producer_info2 &info) {
//                info.owner = owner;
//                info.last_votepay_share_update = ct;
//            });
//        }

        // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
        // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
        // This is okay because in this case the producer will not get paid anything either way.
        // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.

        int64_t producer_per_block_pay = 0;
        if (_gstate.total_unpaid_blocks > 0) {
            producer_per_block_pay = (_gstate.perblock_bucket * prod.unpaid_blocks) / _gstate.total_unpaid_blocks;
        }

        _gstate.perblock_bucket -= producer_per_block_pay;
        _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

        _producers.modify(prod, same_payer, [&](auto &p) {
            p.last_claim_time = ct;
            p.unpaid_blocks = 0;
        });

        if (producer_per_block_pay > 0) {
            INLINE_ACTION_SENDER(eosio::token, transfer)(
                    token_account, {{bpay_account, active_permission},
                                    {owner,        active_permission}},
                    {bpay_account, owner, asset(producer_per_block_pay, core_symbol()),
                     std::string("producer block pay")}
            );
        }
    }

} //namespace eosiosystem
