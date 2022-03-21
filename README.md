# Binance BTCUSDT Orderbook
### External dependencies
* Boost 1.74
* OpenSSL 1.1.1

### TODOs and bugs:
* split code to smaller files
* `Boost::Beast error: ~WssSession(): close: stream truncated` at the and (looks like something that can be ignored now)
* **support ping-pong (othrewise it will work no more than 15 minutes now) !!!**
* better orderbook storage structure (performance!)
* use proper types for `price_t` and `volume_t`
* support different streams
* read params from config (they are hardcoded now)
* ...
* Profit!
