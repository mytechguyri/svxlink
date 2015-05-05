/**
@file	 Ddr.cpp
@brief   A receiver class to handle digital drop receivers
@author  Tobias Blomberg / SM0SVX
@date	 2014-07-16

This file contains a class that handle local digital drop receivers.

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2004-2014 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sigc++/sigc++.h>

#include <cstring>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <complex>
#include <fstream>
#include <algorithm>
#include <iterator>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncAudioSource.h>
#include <AsyncTcpClient.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "Ddr.h"
#include "WbRxRtlSdr.h"
#include "DdrFilterCoeffs.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace sigc;
using namespace Async;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/

namespace {
  template <class T>
  class Decimator
  {
    public:
      Decimator(void) : dec_fact(0), p_Z(0), taps(0) {}

      Decimator(int dec_fact, const float *coeff, int taps)
        : dec_fact(dec_fact), p_Z(0), taps(taps)
      {
        setDecimatorParams(dec_fact, coeff, taps);
      }

      ~Decimator(void)
      {
        delete [] p_Z;
      }

      int decFact(void) const { return dec_fact; }

      void setDecimatorParams(int dec_fact, const float *coeff, int taps)
      {
        set_coeff.assign(coeff, coeff + taps);
        this->dec_fact = dec_fact;
        this->coeff = set_coeff;
        this->taps = taps;

        delete [] p_Z;
        p_Z = new T[taps];
        memset(p_Z, 0, taps * sizeof(*p_Z));
      }

      void setGain(double gain_adjust)
      {
        coeff = set_coeff;
        for (vector<float>::iterator it=coeff.begin(); it!=coeff.end(); ++it)
        {
          *it *= pow(10.0, gain_adjust / 20.0);
        }
      }

      void decimate(vector<T> &out, const vector<T> &in)
      {
        int orig_count = in.size();

          // this implementation assumes in.size() is a multiple of factor_M
        assert(in.size() % dec_fact == 0);
        assert(taps >= dec_fact);

        int num_out = 0;
        typename vector<T>::const_iterator src = in.begin();
        out.clear();
        out.reserve(in.size() / dec_fact);
        while (src != in.end())
        {
            // shift Z delay line up to make room for next samples
          memmove(p_Z + dec_fact, p_Z, (taps - dec_fact) * sizeof(T));

            // copy next samples from input buffer to bottom of Z delay line
          for (int tap = dec_fact - 1; tap >= 0; tap--)
          {
            assert(src != in.end());
            p_Z[tap] = *src++;
          }

            // calculate FIR sum
          T sum(0);
          for (int tap = 0; tap < taps; tap++)
          {
            sum += coeff[tap] * p_Z[tap];
          }
          out.push_back(sum);     /* store sum */
          num_out++;
        }
        assert(num_out == orig_count / dec_fact);
      }

    private:
      int             dec_fact;
      T               *p_Z;
      int             taps;
      vector<float>   set_coeff;
      vector<float>   coeff;
  };

  template <class T>
  class DecimatorMS
  {
    public:
      virtual ~DecimatorMS(void) {}
      virtual int decFact(void) const = 0;
      virtual void decimate(vector<T> &out, const vector<T> &in) = 0;
  };

  template <class T>
  class DecimatorMS1 : public DecimatorMS<T>
  {
    public:
      DecimatorMS1(Decimator<T> &d1) : d1(d1) {}
      virtual int decFact(void) const { return d1.decFact(); }
      virtual void decimate(vector<T> &out, const vector<T> &in)
      {
        d1.decimate(out, in);
      }

    private:
      Decimator<T> &d1;
  };

  template <class T>
  class DecimatorMS2 : public DecimatorMS<T>
  {
    public:
      DecimatorMS2(Decimator<T> &d1, Decimator<T> &d2) : d1(d1), d2(d2) {}
      virtual int decFact(void) const { return d1.decFact() * d2.decFact(); }
      virtual void decimate(vector<T> &out, const vector<T> &in)
      {
        vector<T> dec_samp1;
        d1.decimate(dec_samp1, in);
        d2.decimate(out, dec_samp1);
      }

    private:
      Decimator<T> &d1, &d2;
  };

  template <class T>
  class DecimatorMS3 : public DecimatorMS<T>
  {
    public:
      DecimatorMS3(Decimator<T> &d1, Decimator<T> &d2, Decimator<T> &d3)
        : d1(d1), d2(d2), d3(d3) {}
      virtual int decFact(void) const
      {
        return d1.decFact() * d2.decFact() * d3.decFact();
      }
      virtual void decimate(vector<T> &out, const vector<T> &in)
      {
        vector<T> dec_samp1, dec_samp2;
        d1.decimate(dec_samp1, in);
        d2.decimate(dec_samp2, dec_samp1);
        d3.decimate(out, dec_samp2);
      }

    private:
      Decimator<T> &d1, &d2, &d3;
  };

  template <class T>
  class DecimatorMS4 : public DecimatorMS<T>
  {
    public:
      DecimatorMS4(Decimator<T> &d1, Decimator<T> &d2, Decimator<T> &d3,
                   Decimator<T> &d4)
        : d1(d1), d2(d2), d3(d3), d4(d4) {}
      virtual int decFact(void) const
      {
        return d1.decFact() * d2.decFact() * d3.decFact() * d4.decFact();
      }
      virtual void decimate(vector<T> &out, const vector<T> &in)
      {
        vector<T> dec_samp1, dec_samp2, dec_samp3;
        d1.decimate(dec_samp1, in);
        d2.decimate(dec_samp2, dec_samp1);
        d3.decimate(dec_samp3, dec_samp2);
        d4.decimate(out, dec_samp3);
      }

    private:
      Decimator<T> &d1, &d2, &d3, &d4;
  };

  template <class T>
  class DecimatorMS5 : public DecimatorMS<T>
  {
    public:
      DecimatorMS5(Decimator<T> &d1, Decimator<T> &d2, Decimator<T> &d3,
                   Decimator<T> &d4, Decimator<T> &d5)
        : d1(d1), d2(d2), d3(d3), d4(d4), d5(d5) {}
      virtual int decFact(void) const
      {
        return d1.decFact() * d2.decFact() * d3.decFact() *
               d4.decFact() * d5.decFact();
      }
      virtual void decimate(vector<T> &out, const vector<T> &in)
      {
        vector<T> dec_samp1, dec_samp2, dec_samp3, dec_samp4;
        d1.decimate(dec_samp1, in);
        d2.decimate(dec_samp2, dec_samp1);
        d3.decimate(dec_samp3, dec_samp2);
        d4.decimate(dec_samp4, dec_samp3);
        d5.decimate(out, dec_samp4);
      }

    private:
      Decimator<T> &d1, &d2, &d3, &d4, &d5;
  };

#if 0
  struct HammingWindow
  {
    public:
      HammingWindow(size_t N)
      {
        w = new float[N];
        for (size_t n=0; n<N; ++n)
        {
          w[n] = 0.54 + 0.46 * cos(2*M_PI*n/(N-1));
          //w[n] *= 1.855;
          w[n] *= 1.8519;
        }
      }

      ~HammingWindow(void)
      {
        delete [] w;
      }

      inline float operator[](int i)
      {
        return w[i];
      }

    private:
      float *w;
  };
#endif

  class Demodulator : public Async::AudioSource
  {
    public:
      virtual ~Demodulator(void) {}

      virtual void iq_received(vector<WbRxRtlSdr::Sample> samples) = 0;

      /**
       * @brief Resume audio output to the sink
       * 
       * This function must be reimplemented by the inheriting class. It
       * will be called when the registered audio sink is ready to accept
       * more samples.
       * This function is normally only called from a connected sink object.
       */
      virtual void resumeOutput(void) { }

    protected:
      /**
       * @brief The registered sink has flushed all samples
       *
       * This function should be implemented by the inheriting class. It
       * will be called when all samples have been flushed in the
       * registered sink. If it is not reimplemented, a handler must be set
       * that handle the function call.
       * This function is normally only called from a connected sink object.
       */
      virtual void allSamplesFlushed(void) { }
  };


  class DemodulatorFm : public Demodulator
  {
    public:
      DemodulatorFm(unsigned samp_rate, double max_dev)
        : iold(1.0f), qold(1.0f),
          audio_dec(2, coeff_dec_audio_32k_16k, coeff_dec_audio_32k_16k_cnt),
          wb_mode(false)
      {
        setDemodParams(samp_rate, max_dev);
      }

      void setDemodParams(unsigned samp_rate, double max_dev)
      {
          // Adjust the gain so that the maximum deviation corresponds
          // to a peak audio amplitude of 1.0.
        double adj = static_cast<double>(samp_rate) / (2.0 * M_PI * max_dev);
        adj /= 2.0; // Default to 6dB headroom
        double adj_db = 20.0 * log10(adj);
        audio_dec.setGain(adj_db);

        wb_mode = (samp_rate > 32000);
        if (samp_rate == 160000)
        {
          audio_dec_wb.setDecimatorParams(5, coeff_dec_160k_32k, 
                                          coeff_dec_160k_32k_cnt);
        }
        else if (samp_rate == 192000)
        {
          audio_dec_wb.setDecimatorParams(6, coeff_dec_192k_32k, 
                                          coeff_dec_192k_32k_cnt);
        }
      }

      void iq_received(vector<WbRxRtlSdr::Sample> samples)
      {
          // From article-sdr-is-qs.pdf: Watch your Is and Qs:
          //   FM = (Qn.In-1 - In.Qn-1)/(In.In-1 + Qn.Qn-1)
          //
          // A more indepth report:
          //   Implementation of FM demodulator algorithms on a
          //   high performance digital signal processor
        vector<float> audio;
        for (size_t idx=0; idx<samples.size(); ++idx)
        {
#if 1
          complex<float> samp = samples[idx];
#else
          double fm = 941.0;
          complex<double> samp = exp(
              complex<float>(0,
                (1200/fm)*cos(2.0*M_PI*fm*t) +
                (1200/1633)*cos(2.0*M_PI*1633*t)
                )
              );
          t += T;
#endif

            // Normalize signal amplitude
          samp = samp / abs(samp);

#if 1
            // Mixed demodulator (delay demodulator + phase adapter demodulator)
          float i = samp.real();
          float q = samp.imag();
          double demod = atan2(q*iold - i*qold, i*iold + q*qold);
          //demod=demod*(32000/(2.0*M_PI*5000));
          iold = i;
          qold = q;
          //demod = FastArcTan(demod);
#else
            // Complex baseband delay demodulator
          float demod = arg(samp * conj(prev_samp));
          prev_samp = samp;
#endif

          audio.push_back(demod);
        }
#if 0
        for (size_t i=0; i<audio.size(); ++i)
        {
          //g.calc(w[Ncnt] * audio[i]);
          g.calc(audio[i]);
          if (++Ncnt >= N)
          {
            float dev = 5000*2*sqrt(g.magnitudeSquared())/N;
            //dev *= 1.001603;
            cout << dev << endl;
            Ncnt = 0;
            g.reset();
          }
        }
#endif    
        vector<float> dec_audio;
        if (wb_mode)
        {
          vector<float> dec_audio1;
          audio_dec_wb.decimate(dec_audio1, audio);
          audio_dec.decimate(dec_audio, dec_audio1);
        }
        else
        {
          audio_dec.decimate(dec_audio, audio);
        }
#if 0
        for (size_t i=0; i<dec_audio.size(); ++i)
        {
          //g.calc(w[Ncnt] * dec_audio[i]);
          g.calc(dec_audio[i]);
          if (++Ncnt >= N)
          {
            float dev = 5000*2*sqrt(g.magnitudeSquared())/N;
            //dev *= 0.9811;
            cout << dev << endl;
            Ncnt = 0;
            g.reset();
          }
        }
#endif
        sinkWriteSamples(&dec_audio[0], dec_audio.size());
      }

    private:
      float iold;
      float qold;
      //WbRxRtlSdr::Sample prev_samp;
      Decimator<float> audio_dec_wb;
      Decimator<float> audio_dec;
      bool wb_mode;
#if 0
      Goertzel g;
      int N, Ncnt;
      //HammingWindow w;
#endif
#if 0
      double t;
      const double T;
#endif

        // Maximum error 0.0015 radians (0.085944 degrees)
        // Produced another result and did not affect overall CPU% much
      double FastArcTan(double x)
      {
        return M_PI_4*x - x*(fabs(x) - 1)*(0.2447 + 0.0663*fabs(x));
      }
  };


  class DemodulatorAm : public Demodulator
  {
    public:
      DemodulatorAm(void)
        : audio_dec(2, coeff_dec_32k_16k, coeff_dec_32k_16k_cnt)
      {
        audio_dec.setGain(10);
      }

      void iq_received(vector<WbRxRtlSdr::Sample> samples)
      {
        vector<float> audio;
        for (size_t idx=0; idx<samples.size(); ++idx)
        {
          complex<float> samp = samples[idx];
          float demod = abs(samp);
          audio.push_back(demod);
        }
        /*
        vector<float> dec_audio;
        audio_dec.decimate(dec_audio, audio);
        sinkWriteSamples(&dec_audio[0], dec_audio.size());
        */
        sinkWriteSamples(&audio[0], audio.size());
      }

    private:
      Decimator<float> audio_dec;
  };


  class Translate
  {
    public:
      Translate(unsigned samp_rate, float offset)
        : samp_rate(samp_rate), n(0)
      {
        setOffset(offset);
      }

      void setOffset(int offset)
      {
        n = 0;
        exp_lut.clear();
        if (offset == 0)
        {
          return;
        }
        unsigned N = samp_rate / gcd(samp_rate, abs(offset));
        //cout << "### Translate: offset=" << offset << " N=" << N << endl;
        exp_lut.resize(N);
        for (unsigned i=0; i<N; ++i)
        {
          complex<float> e(0.0f, -2.0*M_PI*offset*i/samp_rate);
          exp_lut[i] = exp(e);
        }
      }

      void iq_received(vector<WbRxRtlSdr::Sample> &out,
                       const vector<WbRxRtlSdr::Sample> &in)
      {
        if (exp_lut.size() > 0)
        {
          out.clear();
          out.reserve(in.size());
          vector<WbRxRtlSdr::Sample>::const_iterator it;
          for (it = in.begin(); it != in.end(); ++it)
          {
            out.push_back(*it * exp_lut[n]);
            if (++n == exp_lut.size())
            {
              n = 0;
            }
          }
        }
        else
        {
          out = in;
        }
      }

    private:
      unsigned samp_rate;
      vector<complex<float> > exp_lut;
      unsigned n;

      /**
       * @brief Find the greatest common divisor for two numbers
       * @param dividend The larger number
       * @param divisor The lesser number
       *
       * This function will return the greatest common divisor of the two given
       * numbers. This implementation requires that the dividend is larger than
       * the divisor.
       */
      unsigned gcd(unsigned dividend, unsigned divisor)
      {
        unsigned reminder = dividend % divisor;
        if (reminder == 0)
        {
          return divisor;
        }
        return gcd(divisor, reminder);
      }
  };

  class Channelizer
  {
    public:
      typedef enum
      {
        BW_WIDE, BW_20K, BW_10K, BW_6K
      } Bandwidth;

      virtual ~Channelizer(void) {}
      virtual void setBw(Bandwidth bw) = 0;
      virtual unsigned chSampRate(void) const = 0;
      virtual void iq_received(vector<WbRxRtlSdr::Sample> &out,
                               const vector<WbRxRtlSdr::Sample> &in) = 0;

      sigc::signal<void, const std::vector<RtlTcp::Sample>&> preDemod;
  };

  class Channelizer960 : public Channelizer
  {
    public:
      Channelizer960(void)
        : dec_960k_192k(5, coeff_dec_960k_192k, coeff_dec_960k_192k_cnt),
          dec_192k_64k( 3, coeff_dec_192k_64k,  coeff_dec_192k_64k_cnt ),
          dec_64k_32k(  2, coeff_dec_64k_32k,   coeff_dec_64k_32k_cnt  ),
          dec_192k_48k( 4, coeff_dec_192k_48k,  coeff_dec_192k_48k_cnt ),
          dec_48k_16k(  3, coeff_dec_48k_16k,   coeff_dec_48k_16k_cnt  ),
          ch_filt(      1, coeff_25k_channel,   coeff_25k_channel_cnt  ),
          ch_filt_narr( 1, coeff_12k5_channel,  coeff_12k5_channel_cnt ),
          ch_filt_6k(   1, coeff_ssb_channel,   coeff_ssb_channel_cnt  ),
          dec(0)
      {
        setBw(BW_20K);
      }
      virtual ~Channelizer960(void)
      {
        delete dec;
        dec = 0;
      }

      virtual void setBw(Bandwidth bw)
      {
        delete dec;
        dec = 0;
        switch (bw)
        {
          case BW_WIDE:
            dec = new DecimatorMS1<complex<float> >(dec_960k_192k);
            return;
          case BW_20K:
            dec = new DecimatorMS4<complex<float> >(dec_960k_192k,
                                                    dec_192k_64k,
                                                    dec_64k_32k, 
                                                    ch_filt);
            return;
          case BW_10K:
            dec = new DecimatorMS4<complex<float> >(dec_960k_192k,
                                                    dec_192k_48k,
                                                    dec_48k_16k, 
                                                    ch_filt_narr);
            return;
          case BW_6K:
            dec = new DecimatorMS4<complex<float> >(dec_960k_192k,
                                                    dec_192k_48k,
                                                    dec_48k_16k, 
                                                    ch_filt_6k);
            return;
        }
        assert(!"Channelizer::setBw: Unknown bandwidth");
      }

      virtual unsigned chSampRate(void) const
      {
        return 960000 / dec->decFact();
      }

      virtual void iq_received(vector<WbRxRtlSdr::Sample> &out,
                               const vector<WbRxRtlSdr::Sample> &in)
      {
        dec->decimate(out, in);
        preDemod(out);
      }

    private:
      Decimator<complex<float> >    dec_960k_192k;
      Decimator<complex<float> >    dec_192k_64k;
      Decimator<complex<float> >    dec_64k_32k;
      Decimator<complex<float> >    dec_192k_48k;
      Decimator<complex<float> >    dec_48k_16k;
      Decimator<complex<float> >    ch_filt;
      Decimator<complex<float> >    ch_filt_narr;
      Decimator<complex<float> >    ch_filt_6k;
      DecimatorMS<complex<float> >  *dec;
  };

  class Channelizer2400 : public Channelizer
  {
    public:
      Channelizer2400(void)
        : dec_2400k_800k(3, coeff_dec_2400k_800k, coeff_dec_2400k_800k_cnt),
          dec_800k_160k (5, coeff_dec_800k_160k,  coeff_dec_800k_160k_cnt ),
          dec_160k_32k  (5, coeff_dec_160k_32k,   coeff_dec_160k_32k_cnt  ),
          dec_32k_16k   (2, coeff_dec_32k_16k,    coeff_dec_32k_16k_cnt   ),
          ch_filt       (1, coeff_25k_channel,    coeff_25k_channel_cnt   ),
          ch_filt_narr  (1, coeff_12k5_channel,   coeff_12k5_channel_cnt  ),
          ch_filt_6k    (1, coeff_ssb_channel,    coeff_ssb_channel_cnt   )
      {
        setBw(BW_20K);
      }
      virtual ~Channelizer2400(void)
      {
        delete dec;
        dec = 0;
      }

      virtual void setBw(Bandwidth bw)
      {
        delete dec;
        dec = 0;

        switch (bw)
        {
          case BW_WIDE:
            dec = new DecimatorMS2<complex<float> >(dec_2400k_800k,
                                                    dec_800k_160k);
            return;
          case BW_20K:
            dec = new DecimatorMS4<complex<float> >(dec_2400k_800k,
                                                    dec_800k_160k,
                                                    dec_160k_32k, 
                                                    ch_filt);
            return;
          case BW_10K:
            dec = new DecimatorMS5<complex<float> >(dec_2400k_800k,
                                                    dec_800k_160k,
                                                    dec_160k_32k, 
                                                    dec_32k_16k,
                                                    ch_filt_narr);
            return;
          case BW_6K:
            dec = new DecimatorMS5<complex<float> >(dec_2400k_800k,
                                                    dec_800k_160k,
                                                    dec_160k_32k, 
                                                    dec_32k_16k,
                                                    ch_filt_6k);
            return;
        }
        assert(!"Channelizer::setBw: Unknown bandwidth");
      }

      virtual unsigned chSampRate(void) const
      {
        return 2400000 / dec->decFact();
      }

      virtual void iq_received(vector<WbRxRtlSdr::Sample> &out,
                               const vector<WbRxRtlSdr::Sample> &in)
      {
        dec->decimate(out, in);
        preDemod(out);
      }

    private:
      Decimator<complex<float> >    dec_2400k_800k;
      Decimator<complex<float> >    dec_800k_160k;
      Decimator<complex<float> >    dec_160k_32k;
      Decimator<complex<float> >    dec_32k_16k;
      Decimator<complex<float> >    ch_filt;
      Decimator<complex<float> >    ch_filt_narr;
      Decimator<complex<float> >    ch_filt_6k;
      DecimatorMS<complex<float> >  *dec;
  };

}; /* anonymous namespace */


class Ddr::Channel : public sigc::trackable, public Async::AudioSource
{
  public:
    Channel(int fq_offset, unsigned sample_rate)
      : sample_rate(sample_rate), channelizer(0),
        fm_demod(32000.0, 5000.0), demod(0),
        trans(sample_rate, fq_offset), enabled(true)
    {
    }

    ~Channel(void)
    {
      delete channelizer;
    }

    bool initialize(void)
    {
      if (sample_rate == 2400000)
      {
        channelizer = new Channelizer2400;
      }
      else if (sample_rate == 960000)
      {
        channelizer = new Channelizer960;
      }
      else
      {
        cout << "*** ERROR: Unsupported tuner sampling rate " << sample_rate
             << ". Legal values are: 960000 and 2400000\n";
        return false;
      }
      setModulation(Ddr::MOD_FM);
      channelizer->preDemod.connect(preDemod.make_slot());
      return true;
    }

    void setFqOffset(int fq_offset)
    {
      trans.setOffset(fq_offset);
    }

    void setModulation(Ddr::Modulation mod)
    {
      demod = 0;
      switch (mod)
      {
        case Ddr::MOD_FM:
          channelizer->setBw(Channelizer::BW_20K);
          fm_demod.setDemodParams(channelizer->chSampRate(), 5000);
          demod = &fm_demod;
          break;
        case Ddr::MOD_WBFM:
          channelizer->setBw(Channelizer::BW_WIDE);
          fm_demod.setDemodParams(channelizer->chSampRate(), 75000);
          demod = &fm_demod;
          break;
        case Ddr::MOD_AM:
          channelizer->setBw(Channelizer::BW_10K);
          demod = &am_demod;
          break;
      }
      assert((demod != 0) && "Channel::setModulation: Unknown modulation");
      setHandler(demod);
    }

    unsigned chSampRate(void) const
    {
      return channelizer->chSampRate();
    }

    void iq_received(vector<WbRxRtlSdr::Sample> samples)
    {
      if (enabled)
      {
        vector<WbRxRtlSdr::Sample> translated, channelized;
        trans.iq_received(translated, samples);
        channelizer->iq_received(channelized, translated);
        demod->iq_received(channelized);
      }
    };

    void enable(void)
    {
      enabled = true;
    }

    void disable(void)
    {
      enabled = false;
    }

    bool isEnabled(void) const { return enabled; }

    sigc::signal<void, const std::vector<RtlTcp::Sample>&> preDemod;

  private:
    unsigned sample_rate;
    Channelizer *channelizer;
    DemodulatorFm fm_demod;
    DemodulatorAm am_demod;
    Demodulator *demod;
    Translate trans;
    bool enabled;
}; /* Channel */


/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

Ddr::DdrMap Ddr::ddr_map;


/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

Ddr *Ddr::find(const std::string &name)
{
  DdrMap::iterator it = ddr_map.find(name);
  if (it != ddr_map.end())
  {
    return (*it).second;
  }
  return 0;
} /* Ddr::find */


Ddr::Ddr(Config &cfg, const std::string& name)
  : LocalRxBase(cfg, name), cfg(cfg), channel(0), rtl(0),
    fq(0.0)
{
} /* Ddr::Ddr */


Ddr::~Ddr(void)
{
  if (rtl != 0)
  {
    rtl->unregisterDdr(this);
    rtl = 0;
  }

  DdrMap::iterator it = ddr_map.find(name());
  if (it != ddr_map.end())
  {
    ddr_map.erase(it);
  }

  delete channel;
} /* Ddr::~Ddr */


bool Ddr::initialize(void)
{
  DdrMap::iterator it = ddr_map.find(name());
  if (it != ddr_map.end())
  {
    cout << "*** ERROR: The name for a Digital Drop Receiver (DDR) must be "
         << "unique. There already is a receiver named \"" << name()
         << "\".\n";
    return false;
  }
  ddr_map[name()] = this;

  if (!cfg.getValue(name(), "FQ", fq))
  {
    cerr << "*** ERROR: Config variable " << name() << "/FQ not set\n";
    return false;
  }
  
  string wbrx;
  if (!cfg.getValue(name(), "WBRX", wbrx))
  {
    cerr << "*** ERROR: Config variable " << name()
         << "/WBRX not set\n";
    return false;
  }

  rtl = WbRxRtlSdr::instance(cfg, wbrx);
  if (rtl == 0)
  {
    cout << "*** ERROR: Could not create WBRX " << wbrx
         << " specified in receiver " << name() << endl;
    return false;
  }
  rtl->registerDdr(this);

  channel = new Channel(fq-rtl->centerFq(), rtl->sampleRate());
  if (!channel->initialize())
  {
    cout << "*** ERROR: Could not initialize channel object for receiver "
         << name() << endl;
    delete channel;
    channel = 0;
    return false;
  }
  channel->preDemod.connect(preDemod.make_slot());
  rtl->iqReceived.connect(mem_fun(*channel, &Channel::iq_received));
  rtl->readyStateChanged.connect(readyStateChanged.make_slot());

  string modstr("FM");
  cfg.getValue(name(), "MODULATION", modstr);
  if (modstr == "FM")
  {
    channel->setModulation(MOD_FM);
  }
  else if (modstr == "WBFM")
  {
    channel->setModulation(MOD_WBFM);
  }
  else if (modstr == "AM")
  {
    channel->setModulation(MOD_AM);
  }
  else
  {
    cout << "*** ERROR: Unknown modulation " << modstr
         << " specified in receiver " << name() << endl;
    delete channel;
    channel = 0;
    return false;
  }

  if (!LocalRxBase::initialize())
  {
    delete channel;
    channel = 0;
    return false;
  }

  tunerFqChanged(rtl->centerFq());

  return true;
} /* Ddr:initialize */


void Ddr::tunerFqChanged(uint32_t center_fq)
{
  if (channel == 0)
  {
    return;
  }

  double new_offset = fq - center_fq;
  if (abs(new_offset) > (rtl->sampleRate() / 2)-12500)
  {
    if (channel->isEnabled())
    {
      cout << "*** WARNING: Could not fit DDR " << name() << " into tuner "
           << rtl->name() << endl;
      channel->disable();
    }
    return;
  }
  channel->setFqOffset(new_offset);
  channel->enable();
} /* Ddr::tunerFqChanged */


void Ddr::setModulation(Modulation mod)
{
  channel->setModulation(mod);
} /* Ddr::setModulation */


unsigned Ddr::preDemodSampleRate(void) const
{
  return channel->chSampRate();
} /* Ddr::preDemodSampleRate */


bool Ddr::isReady(void) const
{
  return (rtl != 0) && rtl->isReady();
} /* Ddr::isReady */



/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/

bool Ddr::audioOpen(void)
{
  return true;
} /* Ddr::audioOpen */


void Ddr::audioClose(void)
{
} /* Ddr::audioClose */


int Ddr::audioSampleRate(void)
{
  return 16000;
} /* Ddr::audioSampleRate */


Async::AudioSource *Ddr::audioSource(void)
{
  return channel;
} /* Ddr::audioSource */



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/


/*
 * This file has not been truncated
 */

