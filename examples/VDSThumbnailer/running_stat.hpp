// running_stat.hpp
// https://www.johndcook.com/blog/standard_deviation/

#include <math.h>


/*

// init - just start everything at 0
// avoid a check on n everytime we run
m_n = 1.
m_oldS = m_oldM = m_newM = 0.;

// add value (x)
m_n++;
m_newM = m_oldM + (x - m_oldM)/m_n;
m_newS = m_oldS + (x - m_oldM)*(x - m_newM);
m_oldM = m_newM; 
m_oldS = m_newS;

// compute mean
double mean() {
  return m_newM;
}

// compute std dev
double std_dev() {
  if(m_n==1) return 0.0;
  return sqrt(m_newS/(m_n - 1));
}

*/

void compute_mean_std_dev(const float *data, const uint count, float& mean, float& std_dev) {
  // vars
  u_int32_t n = 1.;
  double oldS = 0., newS = 0., oldM = 0., newM = 0.;
  const float *pos = data;

  // over the passed data
  while (n<=count) {
    // add value (x)
    float x = *pos++;

    // ignore crazy values
    if (fabs(x)>10e10) {
        x = 0.f;            // count as 0.f or skip ?
    }

    // include another sample
    n++;
    newM = oldM + (x - oldM)/n;
    newS = oldS + (x - oldM)*(x - newM);
    oldM = newM; 
    oldS = newS;
  }

  // assign mean and std dev
  mean = newM;
  std_dev = (n>1) ? sqrt(newS/(n - 1)) : 0.f;
}


/*

        double Variance() const
        {
            return ( (m_n > 1) ? m_newS/(m_n - 1) : 0.0 );


        void Push(double x)
        {
            m_n++;

            // See Knuth TAOCP vol 2, 3rd edition, page 232
            if (m_n == 1)
            {
                m_oldM = m_newM = x;
                m_oldS = 0.0;
            }
            else
            {
                m_newM = m_oldM + (x - m_oldM)/m_n;
                m_newS = m_oldS + (x - m_oldM)*(x - m_newM);
    
                // set up for next iteration
                m_oldM = m_newM; 
                m_oldS = m_newS;
            }
        }

        int NumDataValues() const
        {
            return m_n;
        }

        double Mean() const
        {
            return (m_n > 0) ? m_newM : 0.0;
        }

        double Variance() const
        {
            return ( (m_n > 1) ? m_newS/(m_n - 1) : 0.0 );
        }

        double StandardDeviation() const
        {
            return sqrt( Variance() );
        }

*/





class RunningStat
    {
    public:
        RunningStat() : m_n(0) {}

        void Clear()
        {
            m_n = 0;
        }

        void Push(double x)
        {
            m_n++;

            // See Knuth TAOCP vol 2, 3rd edition, page 232
            if (m_n == 1)
            {
                m_oldM = m_newM = x;
                m_oldS = 0.0;
            }
            else
            {
                m_newM = m_oldM + (x - m_oldM)/m_n;
                m_newS = m_oldS + (x - m_oldM)*(x - m_newM);
    
                // set up for next iteration
                m_oldM = m_newM; 
                m_oldS = m_newS;
            }
        }

        int NumDataValues() const
        {
            return m_n;
        }

        double Mean() const
        {
            return (m_n > 0) ? m_newM : 0.0;
        }

        double Variance() const
        {
            return ( (m_n > 1) ? m_newS/(m_n - 1) : 0.0 );
        }

        double StandardDeviation() const
        {
            return sqrt( Variance() );
        }

    private:
        int m_n;
        double m_oldM, m_newM, m_oldS, m_newS;
    };