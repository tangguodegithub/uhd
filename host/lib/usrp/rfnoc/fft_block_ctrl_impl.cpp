//
// Copyright 2014-2015 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/usrp/rfnoc/fft_block_ctrl.hpp>
#include <uhd/convert.hpp>
#include <uhd/utils/msg.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

using namespace uhd::rfnoc;

class fft_block_ctrl_impl : public fft_block_ctrl
{
public:
    UHD_RFNOC_BLOCK_CONSTRUCTOR(fft_block_ctrl),
        _output_type("sc16"), // Output is either 
        _bpi(uhd::convert::get_bytes_per_item("sc16"))
    {
        // TODO: Remove this reset. Currently used as a workaround due to deal
        //       with the fact the FFT can receive packets that are not
        //       fft_size and can put the FFT core in a bad state that is only
        //       recoverable with a reset.
        reset_fft();
        _fft_reset = get_fft_reset();
        UHD_ASSERT_THROW(_fft_reset == false);

        // Register hooks for magnitude out
        _tree->access<std::string>(_root_path / "args" / "magnitude_out" / "value")
            .subscribe(boost::bind(&fft_block_ctrl_impl::set_magnitude_out_str, this, _1))
            .update()
        ;
        // FFT RFNoC block can be configured to have magnitude output logic not
        // synthesized forcing the magnitude out register to always be 0 regardless if
        // it is set. So, check it's all fine by reading back the register value:
        magnitude_t actual_magnitude_out = get_magnitude_out();
        magnitude_t my_magnitude_out = str_to_mag(get_arg("magnitude_out"));
        UHD_ASSERT_THROW(my_magnitude_out == actual_magnitude_out);

        // Register hooks for spp:
        // This also sets the stream signatures:
        _tree->access<int>(_root_path / "args" / "spp" / "value")
            .subscribe(boost::bind(&fft_block_ctrl_impl::set_fft_size, this, _1))
            .update()
        ;

        _tree->access<std::string>(_root_path / "args" / "otype" / "value")
            .subscribe(boost::bind(&fft_block_ctrl_impl::check_otype, this, _1))
            .publish(boost::bind(&fft_block_ctrl_impl::get_otype_from_magout, this))
            .update()
        ;
    }

    void check_otype(const std::string &otype)
    {
        // See get_otype_from_magout(). Right now, it's always sc16
        if (otype != "sc16") {
            throw uhd::value_error("Invalid output type for FFT block.");
        }
    }

    std::string get_otype_from_magout(void)
    {
        const std::string magout = get_arg("magnitude_out");
        UHD_RFNOC_BLOCK_TRACE() << "magout: " << magout << std::endl;
        if (magout == "COMPLEX") {
            return "sc16";
        }
        if (magout == "MAGNITUDE" or magout == "MAGNITUDE_SQUARED") {
            // Yeah, it's always sc16. But this will probably change soon.
            return "sc16";
        }
        UHD_THROW_INVALID_CODE_PATH();
    }

    void reset_fft()
    {
        set_fft_reset(true);
        set_fft_reset(false);
    }

    void set_fft_reset(bool enable)
    {
        sr_write("FFT_RESET", enable);
    }

    bool get_fft_reset()
    {
        return(user_reg_read64("RB_FFT_RESET") != 0);
    }

    void set_fft_size(int fft_size)
    {
        UHD_RFNOC_BLOCK_TRACE() << "fft_block::set_fft_size()" << std::endl;
        //// 1. Sanity checks
        // Check fft_size is within bounds
        if (fft_size < 16 or fft_size > 4096) {
            // TODO read this bounds from the prop tree (block def)
            throw uhd::value_error("FFT size must be a power of two and within [16, 4096]");
        }
        boost::uint32_t log2_fft_size = 0;
        // Calculate log2(fft_size) and make sure fft_size is a power of 2
        while ( (fft_size & 1) == 0 and (fft_size > 1) ) {
            fft_size >>= 1;
            log2_fft_size++;
        }
        if (fft_size != 1) {
            // Not a power of 2
            throw uhd::value_error("FFT size must be a power of two and within [16, 4096]");
        }

        //// 2. Update block
        // TODO FFT scaling set conservatively (1/N), need method to allow user to set
        sr_write(AXIS_CONFIG_BUS, (0x6AA << 9) + (0 << 8) + log2_fft_size);
        sr_write("FFT_SIZE_LOG2", log2_fft_size);
    } /* set_fft_size() */

    void set_magnitude_out_str(const std::string &magnitude_out)
    {
        if (not (magnitude_out == "COMPLEX" or magnitude_out == "MAGNITUDE" or magnitude_out == "MAGNITUDE_SQUARED")) {
            throw uhd::value_error(str(
                        boost::format("Invalid magnitude_out value: %s")
                        % magnitude_out
            ));

        }
        set_magnitude_out(str_to_mag(magnitude_out));
    }

    void set_magnitude_out(magnitude_t magnitude_out)
    {
        sr_write("MAGNITUDE_OUT", magnitude_out);
    } /* set_magnitude_out() */

    magnitude_t get_magnitude_out()
    {
        return (static_cast<magnitude_t>(user_reg_read64("RB_MAGNITUDE_OUT")));
    }

private:
    magnitude_t str_to_mag(const std::string &magnitude_out)
    {
        // Try int version:
        try {
            size_t mag_out = boost::lexical_cast<size_t>(magnitude_out);
            if (mag_out <= 2) {
                return static_cast<magnitude_t>(mag_out);
            }
        } catch (const boost::bad_lexical_cast &e) {
            // OK, that didn't work
        }

        // Try string version:
        std::string mag_out_upper = boost::to_upper_copy(magnitude_out);
        if (mag_out_upper == "COMPLEX") {
            return COMPLEX;
        } else if (mag_out_upper == "MAGNITUDE") {
            return MAGNITUDE;
        } else if (mag_out_upper == "MAGNITUDE_SQUARED") {
            return MAGNITUDE_SQUARED;
        }

        throw uhd::runtime_error("Invalid magnitude_out value.");
        return COMPLEX;
    }

    std::string _output_type;
    //! Bytes per item (bytes per sample)
    const size_t _bpi;
    bool _fft_reset;
};

UHD_RFNOC_BLOCK_REGISTER(fft_block_ctrl, "FFT");
