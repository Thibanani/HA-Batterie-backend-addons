/*
 * Client qui regroupe les petites ecritures avant de les transmettre.
 *
 * ArduinoHttpClient ecrit la requete en une quinzaine de print() : sans tampon,
 * chacun deviendrait un enregistrement TLS, et en GSM un echange AT avec le
 * modem et un paquet facture. Ici la requete part en un seul envoi, au plus
 * tard au moment ou la reponse commence a etre lue.
 */
#pragma once
#include <Arduino.h>
#include <Client.h>

template <size_t N>
class BufferedClient : public Client {
 public:
  explicit BufferedClient(Client& inner) : inner_(inner) {}

  int connect(IPAddress ip, uint16_t port) override { len_ = 0; return inner_.connect(ip, port); }
  int connect(const char* host, uint16_t port) override { len_ = 0; return inner_.connect(host, port); }

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* buf, size_t size) override {
    size_t done = 0;
    while (done < size) {
      if (len_ == N && !sendPending()) break;
      size_t n = size - done;
      if (n > N - len_) n = N - len_;
      memcpy(buf_ + len_, buf + done, n);
      len_ += n;
      done += n;
    }
    return done;
  }

  int available() override { sendPending(); return inner_.available(); }
  int read() override { sendPending(); return inner_.read(); }
  int read(uint8_t* buf, size_t size) override { sendPending(); return inner_.read(buf, size); }
  int peek() override { sendPending(); return inner_.peek(); }
  // N'appelle pas inner_.flush() : selon les clients, il jette les donnees recues.
  void flush() override { sendPending(); }
  void stop() override { len_ = 0; inner_.stop(); }
  uint8_t connected() override { return inner_.connected(); }
  operator bool() override { return static_cast<bool>(inner_); }

 private:
  // Transmet le tampon ; en cas d'echec la requete est perdue et la lecture de
  // la reponse echouera (timeout), ce que l'appelant traite deja.
  bool sendPending() {
    if (len_ == 0) return true;
    size_t sent = inner_.write(buf_, len_);
    bool ok = (sent == len_);
    len_ = 0;
    return ok;
  }

  Client& inner_;
  uint8_t buf_[N];
  size_t  len_ = 0;
};
