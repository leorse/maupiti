// Décodeur d'images Lankhor — voir lankhor_image.hpp (GPL v3+, dérivé de ScummVM).
#include "lankhor_image.hpp"

#include <algorithm>
#include <stdexcept>

namespace maupiti {
namespace {

constexpr int kWidth = 320;          // DEFAULT_WIDTH dans ScummVM
constexpr int kBuf = 65536;
constexpr int kMargin = 8192;        // garde-fou contre les débordements de pointeur
constexpr int kBufferSize = 40000;

uint16_t be16(std::span<const uint8_t> d, size_t o) {
    if (o + 1 >= d.size()) throw std::runtime_error("image tronquée");
    return static_cast<uint16_t>((d[o] << 8) | d[o + 1]);
}

class Decoder {
public:
    explicit Decoder(std::span<const uint8_t> src) : src_(src) {}

    Image decode() {
        auto& s = src_;
        if (s.size() < 4) throw std::runtime_error("image trop courte");
        Image img;
        img.transparent = s[0] == 0 ? 16 : s[2];
        const bool offset_flag = s[0] == 0;
        const int entry_count = s[1];
        size_t pos = 4;
        if (offset_flag) {
            std::array<uint16_t, 16> pal{};
            pal[0] = be16(s, 2);
            for (int i = 0; i < 15; ++i) pal[i + 1] = be16(s, 4 + 2 * i);
            img.palette = pal;
            pos += 30;
        }
        if (entry_count == 0) throw std::runtime_error("image vide");

        size_t p = pos;
        int ox = 999, oy = 999;
        for (int i = 0; i < entry_count; ++i) {
            ox = std::min<int>(ox, be16(s, p + 4));
            oy = std::min<int>(oy, be16(s, p + 6));
            size_t size = be16(s, p) + be16(s, p + 2);
            if (size & 1) ++size;
            p += size + 14;
        }
        out_.assign(kBuf + 2 * kMargin, static_cast<uint8_t>(img.transparent));
        lookup_table_.assign(kBufferSize, 0);
        src_buffer_.assign(kBufferSize, 0);

        size_t cur = pos;
        size_t ps_start = pos;
        w_ = h_ = 0;
        for (int entry = 0; entry < entry_count; ++entry) {
            mem_ = s;
            ps_ = cur;
            const int lookup_bytes = be16(s, ps_);
            const int src_size = be16(s, ps_ + 2);
            xp_ = be16(s, ps_ + 4) - ox;
            yp_ = be16(s, ps_ + 6) - oy;
            if (xp_ < 0 || yp_ < 0 || xp_ >= 640 || yp_ >= 400) throw std::runtime_error("position invalide");
            ps_ += 8;
            const int decom_code = be16(s, ps_);
            x_size_ = be16(s, ps_ + 2) + 1;
            y_size_ = be16(s, ps_ + 4) + 1;
            // boîte englobante de tous les morceaux (les visages Atari ne forment pas une grille régulière)
            w_ = std::max(w_, xp_ + x_size_);
            h_ = std::max(h_, yp_ + y_size_);
            ps_ += 6;
            pd_ = 0;
            lookup_index_ = 0;
            nibble_flag_ = false;
            int decom_index = 0;
            pl_ = -1;
            if (decom_code >> 8) {
                int table_offset = 0;
                if (decom_code & 1) {
                    do {
                        int outer = desanalyse();
                        const int inner = desanalyse();
                        const size_t saved_ps = ps_;
                        const bool saved_nf = nibble_flag_;
                        const int saved_li = lookup_index_;
                        do {
                            ps_ = saved_ps;
                            nibble_flag_ = saved_nf;
                            lookup_index_ = saved_li;
                            for (int i = 0; i < inner; ++i) put_table(lookup_table_, table_offset++, next_nibble());
                        } while (--outer > 0);
                    } while (lookup_index_ < lookup_bytes - 1);
                } else {
                    for (int i = 0; i < lookup_bytes * 2; ++i) put_table(lookup_table_, i, next_nibble());
                }
                if (nibble_flag_) {
                    ++ps_;
                    nibble_flag_ = false;
                }
                if ((lookup_bytes + src_size) & 1) ++ps_;
                table_offset = 0;
                lookup_index_ = 0;
                if (decom_code & 2) {
                    do {
                        int outer = desanalyse();
                        const int inner = desanalyse();
                        lookup_index_ += inner;
                        if (nibble_flag_) {
                            ++ps_;
                            ++lookup_index_;
                            nibble_flag_ = false;
                        }
                        const size_t start = ps_;
                        do {
                            ps_ = start;
                            for (int i = 0; i < inner; ++i) put_table(src_buffer_, table_offset++, at(ps_++));
                        } while (--outer > 0);
                    } while (lookup_index_ < src_size - 1);
                } else {
                    for (int i = 0; i < src_size; ++i) put_table(src_buffer_, i, at(ps_++));
                }
                if (nibble_flag_) ++ps_;
                ps_start = ps_;
                pd_ = yp_ * kWidth + xp_;
                mem_ = std::span<const uint8_t>(src_buffer_);
                ps_ = 0;
                pl_ = -1;
                lookup_value_ = lookup_index_ = 0;
                nibble_flag_ = false;
                decom_index = decom_code >> 8;
            }
            lookup_value_ = 0;
            run_method(decom_index, lookup_bytes, ps_start);
            cur = ps_start;
        }
        if (w_ <= 0 || h_ <= 0) throw std::runtime_error("taille d'image nulle");
        img.width = w_;
        img.height = h_;
        img.x = ox;
        img.y = oy;
        img.pixels.resize(static_cast<size_t>(w_) * h_);
        for (int y = 0; y < h_; ++y)
            std::copy_n(out_.begin() + kMargin + y * kWidth, w_, img.pixels.begin() + static_cast<size_t>(y) * w_);
        // Les quartets sont dans l'ordre des plans de bits Atari : l'indice de palette
        // s'obtient en inversant les 4 bits (table identique dans maup.exe, DS:0xEA6).
        static constexpr uint8_t kBitrev[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        for (auto& p : img.pixels)
            if (p < 16) p = kBitrev[p];
        if (img.transparent < 16) img.transparent = kBitrev[img.transparent];
        return img;
    }

private:
    std::span<const uint8_t> src_;
    std::span<const uint8_t> mem_;
    std::vector<uint8_t> out_, lookup_table_, src_buffer_;
    size_t ps_ = 0;
    int pd_ = 0, pl_ = -1;
    int xp_ = 0, yp_ = 0, x_size_ = 0, y_size_ = 0, w_ = 0, h_ = 0;
    int lookup_index_ = 0, lookup_value_ = 0;
    bool nibble_flag_ = false;
    int thickness_ = 0, x_inc_ = 0, y_inc_ = 0, x_end_ = 0, y_end_ = 0;

    uint8_t at(size_t i) const {
        if (i >= mem_.size()) throw std::runtime_error("lecture hors des données");
        return mem_[i];
    }
    static void put_table(std::vector<uint8_t>& t, int i, uint8_t v) {
        if (i < 0 || i >= static_cast<int>(t.size())) throw std::runtime_error("table de décompression pleine");
        t[i] = v;
    }
    void put(uint8_t v) {
        const int i = pd_ + kMargin;
        if (i < 0 || i >= static_cast<int>(out_.size())) throw std::runtime_error("écriture hors image");
        out_[i] = v;
    }
    uint8_t next_nibble() {
        const uint8_t v = at(ps_);
        if (nibble_flag_) {
            ++ps_;
            ++lookup_index_;
            nibble_flag_ = false;
            return v & 0xF;
        }
        nibble_flag_ = true;
        return v >> 4;
    }
    uint8_t next_byte() {
        while (!lookup_value_) {
            int v;
            do {
                v = next_nibble();
                lookup_value_ += v;
            } while (v == 0xF);
            ++pl_;
        }
        --lookup_value_;
        if (pl_ < 0 || pl_ >= static_cast<int>(lookup_table_.size())) throw std::runtime_error("motif hors table");
        return lookup_table_[pl_];
    }
    int desanalyse() {
        int total = 0;
        int v = next_nibble();
        if (v == 0xF) {
            int v2;
            do {
                v2 = next_nibble();
                total += v2;
            } while (v2 == 0xF);
            total *= 15;
            v = next_nibble();
        }
        return total + v;
    }
    void incr_xsize() {
        if (x_size_ & 1) ++x_size_;
    }
    void next_ptr() { pd_ += x_inc_ + y_inc_; }

    void run_method(int m, int lookup_bytes, size_t& ps_start) {
        switch (m) {
        case 0: {
            pd_ = yp_ * kWidth + xp_;
            const size_t start = ps_;
            incr_xsize();
            for (int y = 0; y < y_size_; ++y, pd_ += kWidth) {
                const int line = pd_;
                for (int x = 0; x < x_size_; ++x) {
                    pd_ = line + x;
                    put(next_nibble());
                }
                pd_ = line;
            }
            ps_start = start + lookup_bytes + (lookup_bytes & 1);
            break;
        }
        case 1:
            incr_xsize();
            for (int y = 0; y < y_size_; ++y) {
                if (y % 2 == 0)
                    for (int x = 0; x < x_size_; ++x) { put(next_byte()); ++pd_; }
                else
                    for (int x = 0; x < x_size_; ++x) { --pd_; put(next_byte()); }
                pd_ += kWidth;
            }
            break;
        case 2:
            for (int x = 0; x < x_size_; ++x) {
                if (x % 2 == 0)
                    for (int y = 0; y < y_size_; ++y) { put(next_byte()); pd_ += kWidth; }
                else
                    for (int y = 0; y < y_size_; ++y) { pd_ -= kWidth; put(next_byte()); }
                ++pd_;
            }
            break;
        case 3: thickness_ = 2; horizontal(); break;
        case 4: thickness_ = 2; vertical(); break;
        case 5: thickness_ = 3; horizontal(); break;
        case 6: thickness_ = 4; vertical(); break;
        case 7:
            incr_xsize();
            for (int y = 0; y < y_size_; ++y, pd_ += kWidth) {
                const int line = pd_;
                for (int x = 0; x < x_size_; ++x) { pd_ = line + x; put(next_byte()); }
                pd_ = line;
            }
            break;
        case 8:
            for (int x = 0; x < x_size_; ++x, ++pd_) {
                const int col = pd_;
                for (int y = 0; y < y_size_; ++y) { pd_ = col + y * kWidth; put(next_byte()); }
                pd_ = col;
            }
            break;
        case 9: thickness_ = 4; horizontal(); break;
        case 10: thickness_ = 6; horizontal(); break;
        case 11: decom11(); break;
        case 12:
            incr_xsize();
            thickness_ = x_inc_ = 1; y_inc_ = kWidth; y_end_ = y_size_; x_end_ = x_size_;
            diag();
            break;
        case 13:
            incr_xsize();
            thickness_ = x_size_; y_inc_ = 1; y_end_ = x_size_; x_inc_ = kWidth; x_end_ = y_size_;
            diag();
            break;
        case 14:
            thickness_ = y_inc_ = 1; y_end_ = x_size_; x_inc_ = kWidth; x_end_ = y_size_;
            diag();
            break;
        case 15:
            incr_xsize();
            thickness_ = 2; y_inc_ = kWidth; y_end_ = y_size_; x_inc_ = 1; x_end_ = x_size_;
            diag();
            break;
        case 16:
            thickness_ = 3; y_inc_ = 1; y_end_ = x_size_; x_inc_ = kWidth; x_end_ = y_size_;
            diag();
            break;
        case 17:
            incr_xsize();
            thickness_ = 3; y_inc_ = kWidth; y_end_ = y_size_; x_inc_ = 1; x_end_ = x_size_;
            diag();
            break;
        case 18:
            incr_xsize();
            thickness_ = 5; y_inc_ = kWidth; y_end_ = y_size_; x_inc_ = 1; x_end_ = x_size_;
            diag();
            break;
        default:
            throw std::runtime_error("méthode de compression inconnue");
        }
    }

    void horizontal() {
        incr_xsize();
        const int end = pd_ + (y_size_ - 1) * kWidth + x_size_;
        for (;;) {
            if ((thickness_ - 1) * kWidth + pd_ >= end) {
                if (--thickness_ == 0) break;
                continue;
            }
            bool cont = false;
            do {
                for (int xi = 0; xi < x_size_; ++xi) {
                    if (xi % 2 == 0) {
                        if (xi) ++pd_;
                        for (int i = 0; i < thickness_; ++i) { put(next_byte()); pd_ += kWidth; }
                        ++pd_;
                    } else {
                        for (int i = 0; i < thickness_; ++i) { pd_ -= kWidth; put(next_byte()); }
                    }
                }
                if (x_size_ % 2 == 0) {
                    int bs = thickness_ * kWidth;
                    pd_ += bs;
                    bs -= kWidth;
                    if (end < pd_ + bs) {
                        do {
                            if (--thickness_ == 0) return;
                        } while (pd_ + (thickness_ - 1) * kWidth >= end);
                    }
                } else {
                    while (pd_ + (thickness_ - 1) * kWidth >= end)
                        if (--thickness_ == 0) return;
                }
                for (int xi = 0; xi < x_size_; ++xi, --pd_) {
                    if (xi % 2 == 0)
                        for (int i = 0; i < thickness_; ++i) { put(next_byte()); pd_ += kWidth; }
                    else
                        for (int i = 0; i < thickness_; ++i) { pd_ -= kWidth; put(next_byte()); }
                }
                if (x_size_ % 2 == 1) {
                    ++pd_;
                    if (pd_ + (thickness_ - 1) * kWidth < end) { cont = true; break; }
                } else {
                    pd_ += thickness_ * kWidth + 1;
                    cont = true;
                    break;
                }
                ++pd_;
            } while ((thickness_ - 1) * kWidth + pd_ < end);
            if (cont) continue;
            if (--thickness_ == 0) break;
        }
    }

    void vertical() {
        int draw = 0;
        for (;;) {
            while (draw + thickness_ > x_size_)
                if (--thickness_ == 0) return;
            for (int y = 0; y < y_size_; ++y) {
                if (y % 2 == 0) {
                    if (y > 0) pd_ += kWidth;
                    draw += thickness_;
                    for (int i = 0; i < thickness_; ++i) { put(next_byte()); ++pd_; }
                } else {
                    pd_ += kWidth;
                    draw -= thickness_;
                    for (int i = 0; i < thickness_; ++i) { --pd_; put(next_byte()); }
                }
            }
            if (y_size_ % 2 == 0) { pd_ += thickness_; draw += thickness_; }
            while (x_size_ < draw + thickness_)
                if (--thickness_ == 0) return;
            for (int y = 0; y < y_size_; ++y) {
                if (y % 2 == 0) {
                    if (y > 0) pd_ -= kWidth;
                    draw += thickness_;
                    for (int i = 0; i < thickness_; ++i) { put(next_byte()); ++pd_; }
                } else {
                    pd_ -= kWidth;
                    draw -= thickness_;
                    for (int i = 0; i < thickness_; ++i) { --pd_; put(next_byte()); }
                }
            }
            if (y_size_ % 2 == 0) { pd_ += thickness_; draw += thickness_; }
        }
    }

    void decom11() {
        int ypos = 0, draw = 0;
        y_inc_ = kWidth;
        x_inc_ = -1;
        --x_size_;
        --y_size_;
        int area = 0;
        while (area != -1) {
            switch (area) {
            case 0:
                put(next_byte());
                area = 1;
                break;
            case 1:
                next_ptr();
                if (!draw) {
                    x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                    if (ypos == y_size_) { next_ptr(); ++draw; } else { ++ypos; }
                    ++pd_;
                    put(next_byte());
                    area = 2;
                } else if (ypos != y_size_) {
                    ++ypos; --draw;
                    area = 0;
                } else {
                    x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                    next_ptr();
                    ++draw;
                    ++pd_;
                    put(next_byte());
                    area = draw == x_size_ ? -1 : 2;
                }
                break;
            case 2:
                next_ptr();
                if (!ypos) {
                    x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                    if (draw == x_size_) { next_ptr(); ++ypos; } else { ++draw; }
                    pd_ += kWidth;
                    area = 0;
                } else if (draw != x_size_) {
                    ++draw; --ypos;
                    put(next_byte());
                    area = 2;
                } else {
                    pd_ += kWidth;
                    ++ypos;
                    x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                    next_ptr();
                    put(next_byte());
                    area = ypos == y_size_ ? -1 : 1;
                }
                break;
            default:
                break;
            }
        }
    }

    bool tfp(int v) {
        const int diff = y_end_ - v;
        if (!diff) return true;
        if (diff < thickness_ + 1) thickness_ = diff - 1;
        return false;
    }
    int tf1(int v) {
        v += thickness_ + 1;
        pd_ += (thickness_ + 1) * y_inc_;
        return v;
    }
    int tf2(int v) {
        v += thickness_ + 1;
        for (int i = 0; i <= thickness_; ++i) { put(next_byte()); pd_ += y_inc_; }
        return v;
    }

    void diag() {
        int diag_index = 0, draw = 0;
        --x_end_;
        while (!tfp(diag_index)) {
            for (;;) {
                x_inc_ = -x_inc_;
                for (int i = 0; i <= thickness_; ++i) { put(next_byte()); x_inc_ = -x_inc_; next_ptr(); }
                y_inc_ = -y_inc_;
                pd_ += y_inc_;
                for (int i = 0; i <= thickness_; ++i) { put(next_byte()); x_inc_ = -x_inc_; next_ptr(); }
                x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                next_ptr();
                ++draw;
                if (x_end_ < draw + 1) { diag_index = tf1(diag_index); break; }
                pd_ += x_inc_;
                ++draw;
                if (x_end_ < draw + 1) { diag_index = tf2(diag_index); break; }
            }
            if (tfp(diag_index)) break;
            for (;;) {
                for (int i = 0; i <= thickness_; ++i) { put(next_byte()); x_inc_ = -x_inc_; next_ptr(); }
                y_inc_ = -y_inc_;
                pd_ += y_inc_;
                for (int i = 0; i <= thickness_; ++i) { put(next_byte()); x_inc_ = -x_inc_; next_ptr(); }
                x_inc_ = -x_inc_; y_inc_ = -y_inc_;
                next_ptr();
                if (--draw == 0) { diag_index = tf1(diag_index); x_inc_ = -x_inc_; break; }
                pd_ += x_inc_;
                if (--draw == 0) { diag_index = tf2(diag_index); x_inc_ = -x_inc_; break; }
                x_inc_ = -x_inc_;
            }
        }
    }
};

}  // namespace

Image decode_image(std::span<const uint8_t> data) { return Decoder(data).decode(); }

size_t image_size(std::span<const uint8_t> d) {
    if (d.size() < 4) throw std::runtime_error("image trop courte");
    const int count = d[1];
    size_t p = 4 + (d[0] == 0 ? 30 : 0);
    for (int i = 0; i < count; ++i) {
        size_t size = be16(d, p) + be16(d, p + 2);
        if (size & 1) ++size;
        p += size + 14;
    }
    return p;
}

std::vector<Image> decode_image_run(std::span<const uint8_t> data) {
    std::vector<Image> out;
    size_t p = 0;
    while (p + 18 <= data.size()) {
        if (data[p] > 1 || data[p + 1] == 0) break;
        try {
            const auto sub = data.subspan(p);
            const size_t n = image_size(sub);
            if (n > sub.size()) break;
            out.push_back(decode_image(sub.first(n)));
            p += n;
        } catch (const std::exception&) {
            break;
        }
    }
    return out;
}

}  // namespace maupiti
