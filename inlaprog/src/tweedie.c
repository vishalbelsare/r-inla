
/* tweedie.c
 * 
 * Copyright (C) 2021-2021 Havard Rue
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The author's contact information:
 *
 *        Haavard Rue
 *        CEMSE Division
 *        King Abdullah University of Science and Technology
 *        Thuwal 23955-6900, Saudi Arabia
 *        Email: haavard.rue@kaust.edu.sa
 *        Office: +966 (0)12 808 0640
 *
 */
#ifndef GITCOMMIT
#define GITCOMMIT
#endif
static const char GitID[] = GITCOMMIT;

#include <math.h>
#include <strings.h>
#if !defined(__FreeBSD__)
#include <malloc.h>
#endif
#include <stdlib.h>

#include "rmath.h"
#undef ISNAN

#include "GMRFLib/GMRFLib.h"
#include "GMRFLib/GMRFLibP.h"
#include "tweedie.h"

// the dtweedie-code is modified from tweedie.c in the (GPL'ed) cplm_0.7-9.tar.gz package of Wayne Zhang
// <actuary_zhang@hotmail.com>, to tailor it to the INLA use.

#define TWEEDIE_DROP 40.0
#define TWEEDIE_INCRE 2
#define TWEEDIE_INCRE_HIGH 32
#define TWEEDIE_MAX_IDX 32768

/**
 * Compute the log density for the tweedie compound Poisson distribution.
 * This is based on the dtweedie.series function in R, but bounds
 * are not determined using all observations because this could result in 
 * dramatically slower computation in certain circumstances. 
 *
 * @param n the number of observations
 * @param y the observation
 * @param mu the vector of means
 * @param phi scalar: the dispersion parameter
 * @param p scalar: the index parameter
 * @param ldens the vector that stores the computed log density
 */

void dtweedie(int n, double y, double *mu, double phi, double p, double *ldens)
{
	// nice simplifications are possible since only 'mu' depends on '[i]' and the rest of the parameters are constants

	static struct {
		int nn_max;
		int idx_low;
		int idx_high;
		double save_phi;
		double save_p;
		double *work;
		double *wwork;
	} store = { -1, -1, -1, -9999.9999, -9999.9999, NULL, NULL};
#pragma omp threadprivate(store)

	if (store.nn_max < 0) {
		store.nn_max = 1024;
		store.work = Calloc(2 * store.nn_max, double);
		store.wwork = store.work + store.nn_max;
	}

	double p1 = p - 1.0, p2 = 2.0 - p;
	double a = -p2 / p1, a1 = 1.0 / p1;
	double cc, j, w, sum_ww = 0.0, ww_max = 0.0;

	int use_interpolation = 1, nterms, k, i;
	int reuse = 1;
	
	// fast return
	if (ISZERO(y)) {
		for (int i = 0; i < n; i++) {
			ldens[i] = -pow(mu[i], p2) / (phi * p2);
		}
		return;
	}


	int jh, jl, jd, jmax;
	double logz, logz_no_y;

	cc = a * log(p1) - log(p2);
	jmax = DMAX(1.0, pow(y, p2) / (phi * p2));
	logz = -a * log(y) - a1 * log(phi) + cc;
	logz_no_y = - a1 * log(phi) + cc;

	// locate upper bound 
	cc = logz + a1 + a * log(-a);
	j = jmax;
	w = a1 * j;
	while (1) {
		j += TWEEDIE_INCRE;
		if (j * (cc - a1 * log(j)) < (w - TWEEDIE_DROP))
			break;
	}
	jh = IMIN(TWEEDIE_MAX_IDX, ceil(j));

	if (0) {
		// locate lower bound 
		j = jmax;
		while (1) {
			j -= TWEEDIE_INCRE;
			if (j < 1 || j * (cc - a1 * log(j)) < w - TWEEDIE_DROP)
				break;
		}
	}
	
	jl = 1;						       /* use this all the time */
	jd = jh - jl + 1;
	nterms = jd;

	if (nterms + TWEEDIE_INCRE_HIGH >= store.nn_max) {
		reuse = 0;
		store.nn_max = (nterms + TWEEDIE_INCRE_HIGH) * 2;
		Free(store.work);
		store.work = Calloc(2 * store.nn_max, double);
		store.wwork = store.work + store.nn_max;
	}

	reuse = reuse && (p ==  store.save_p && phi ==  store.save_phi);
	reuse = reuse && (store.idx_low <= jl && store.idx_high >= jh);

	if (!reuse) {
		jh += TWEEDIE_INCRE_HIGH;
		jd = jh - jl + 1;
		nterms = jd;
				
		if (!use_interpolation) {
			for (k = 0; k < nterms; k++) {
				j = k + jl;
				store.wwork[k] = j * logz_no_y - inla_lgamma_fast(1.0 + j) - inla_lgamma_fast(-a * j);
			}
		} else {
			for (k = 0; k < nterms + 1; k += 2) {
				j = k + jl;
				store.wwork[k] = j * logz_no_y - inla_lgamma_fast(1.0 + j) - inla_lgamma_fast(-a * j);

				if (k > 0) {
					double estimate = 0.5 * (store.wwork[k] + store.wwork[k - 2]);
					double correction = (1.0 - a) * (1.0 / j + 1.0 / SQR(j));
					double limit = 0.05;
					if (ABS(correction) > limit) {
						// correction term to large: skip interpolation
						double jj = j - 1.0;
						store.wwork[k - 1] = jj * logz_no_y - inla_lgamma_fast(1.0 + jj) - inla_lgamma_fast(-a * jj);
					} else {
						// correction term small: do interpolation
						store.wwork[k - 1] = estimate + correction;
					}
				}
			}
		}
		store.save_p = p;
		store.save_phi = phi;
		store.idx_low = jl;
		store.idx_high = jh;
	} 
	
	// assume every 'y' is potential different, but the parameters changes just once in a while
	double term = -a * log(y); // the y-term we have removed from 'logz'
	nterms = store.idx_high - store.idx_low + 1;
			
	sum_ww = 0.0;
	double tmp;

	if (0) {
		ww_max = store.wwork[0] + store.idx_low * term;
		for(k = 0; k < nterms; k++) {
			j = k + store.idx_low;
			store.work[k] = store.wwork[k] + term * j;
			if (store.work[k] > ww_max) {
				ww_max = store.work[k];
			}
		}
		for (k = 0; k < nterms; k++) {
			tmp = store.work[k] - ww_max;
			if (tmp > -14.0) {
				sum_ww += exp(tmp);
			}
		}
	} else {
		int idx_max = 0;
		double lim = -18.0;

		ww_max = store.wwork[0] + store.idx_low * term;
		for(k = 0; k < nterms; k++) {
			j = k + store.idx_low;
			store.work[k] = store.wwork[k] + term * j;
			if (store.work[k] > ww_max) {
				ww_max = store.work[k];
				idx_max = k;
			}
		}

		for (k = idx_max; k < nterms; k++) {	       /* assume there is one mode */
			tmp = store.work[k] - ww_max;
			if (tmp > lim) {
				sum_ww += exp(tmp);
			} else {
				break;
			}
		}
		for (k = idx_max - 1; k >= 0; k--) {
			tmp = store.work[k] - ww_max;
			if (tmp > lim) {
				sum_ww += exp(tmp);
			} else {
				break;
			}
		}
	}

	for (i = 0; i < n; i++) {
		ldens[i] = -pow(mu[i], p2) / (phi * p2) - y / (phi * p1 * pow(mu[i], p1)) - log(y) + log(sum_ww) + ww_max;
	}

	return;
}

double ptweedie(double y, double mu, double phi, double p)
{
	// compute Prob(Y <= y)
	double lambda = pow(mu, 2.0 - p) / (phi * (2.0 - p));
	double alpha = (2.0 - p) / (p - 1.0);
	double gamma = phi * (p - 1.0) * pow(mu, p - 1.0);
	double plim = 0.999;
	double retval, prob, pacc;

	// result from a test: Rmath takes 30% more time than the GSL call to 'pgamma'
	// retval += prob * MATHLIB_FUN(pgamma)(y, n * alpha, gamma, 1, 0);

	// when sd/mean < low, we basically are in the Gaussian regime. this is for the case Gamma(n*alpha, gamma), so mean=
	// n*alpha*gamma, and sd= sqrt(n*alpha)*gamma, so gamma cancel.
	double low = 0.25;
	int n, n_gauss = (int) (1.0 / (alpha * SQR(low)) + 0.5);

	// stride should scale with stdev sqrt(lambda). the below choice gives (about)
	// stride=1 for lam < 5
	// stride=2 for 5 <= lam < 13
	// stride=3 for 13 <= lam < 25
	// stride=4 for 25 <= lam < 41, etc...
	int stride = IMAX(1, (int) (sqrt(lambda / 2.0) + 0.5));

	retval = pacc = prob = gsl_ran_poisson_pdf((unsigned int) 0, lambda);

	if (stride == 1) {
		for (n = 1; pacc < plim; n++, pacc += prob) {
			// prob = gsl_ran_poisson_pdf((unsigned int) n, lambda);
			prob *= lambda / n;

			if (n < n_gauss) {
				retval += prob * gsl_cdf_gamma_P(y, n * alpha, gamma);
			} else {
				retval += prob * inla_Phi_fast((y - (n * alpha * gamma)) / (sqrt(n * alpha) * gamma));
			}
		}
	} else {
		// use log-linear interpolation for the cdf's between each stride
		double cdf, lcdf, lcdf_prev;

		// we have to start manually
		n = 1;
		prob *= lambda / n;
		pacc += prob;
		cdf = gsl_cdf_gamma_P(y, n * alpha, gamma);
		lcdf_prev = log(cdf);
		retval += prob * cdf;

		for (n = stride + 1; pacc < plim; n += stride) {
			if (n < n_gauss) {
				cdf = gsl_cdf_gamma_P(y, n * alpha, gamma);
			} else {
				cdf = inla_Phi_fast((y - (n * alpha * gamma)) / (sqrt(n * alpha) * gamma));
			}
			lcdf = lcdf_prev = log(cdf);

			for (int k = stride - 1; k >= 1; k--) {
				prob *= lambda / (n - k);
				pacc += prob;
				retval += prob * exp((k * lcdf_prev + (stride - k) * lcdf) / (double) stride);
			}

			prob *= lambda / n;
			pacc += prob;
			retval += prob * cdf;
		}
	}

	return (retval);
}