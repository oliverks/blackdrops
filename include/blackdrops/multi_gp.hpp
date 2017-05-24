#ifndef BLACKDROPS_MULTI_GP_HPP
#define BLACKDROPS_MULTI_GP_HPP

#include <limbo/model/gp.hpp>

#include <map>

namespace blackdrops {

    template <typename Params, template <typename, typename, typename, typename> class GPClass, typename KernelFunction, typename MeanFunction, class HyperParamsOptimizer = limbo::model::gp::NoLFOpt<Params>>
    class MultiGP {
    public:
        struct MultiMean : MeanFunction {

            MultiMean(size_t dim_out = 1) : _id(0) {}

            template <typename GP>
            Eigen::VectorXd operator()(const Eigen::VectorXd& v, const GP& gp) const
            {
                Eigen::VectorXd res;
                // if (MultiMean::_cashing.find(v) == MultiMean::_cashing.end())
                res = MeanFunction::operator()(v, gp);
                // else
                //     res = MultiMean::_cashing[v];

                if (_id >= res.size())
                    return res.tail(1);

                return limbo::tools::make_vector(res(_id));
            }

            void set_id(int id)
            {
                _id = id;
            }

            int id() { return _id; }

        protected:
            // static std::map<Eigen::VectorXd, Eigen::VectorXd> _cashing;
            int _id;
        };

        using GP_t = GPClass<Params, KernelFunction, MultiMean, limbo::model::gp::NoLFOpt<Params>>;

        /// useful because the model might be created before knowing anything about the process
        MultiGP() : _dim_in(-1), _dim_out(-1)
        {
        }

        /// useful because the model might be created  before having samples
        MultiGP(int dim_in, int dim_out)
            : _dim_in(dim_in), _dim_out(dim_out)
        {
            _gp_models.resize(_dim_out);
            for (int i = 0; i < _dim_out; i++)
                _gp_models[i] = GP_t(_dim_in, 1);
        }

        /// Compute the GP from samples and observations. This call needs to be explicit!
        void compute(const std::vector<Eigen::VectorXd>& samples,
            const std::vector<Eigen::VectorXd>& observations, bool compute_kernel = true)
        {
            if (_dim_in != samples[0].size()) {
                _dim_in = samples[0].size();
            }

            if (_dim_out != observations[0].size()) {
                _dim_out = observations[0].size();
            }

            if ((int)_gp_models.size() != _dim_out) {
                _gp_models.resize(_dim_out);
                for (int i = 0; i < _dim_out; i++)
                    _gp_models[i] = GP_t(_dim_in, 1);
            }

            std::vector<std::vector<Eigen::VectorXd>> obs(_dim_out);

            for (size_t j = 0; j < observations.size(); j++) {
                for (int i = 0; i < _dim_out; i++) {
                    obs[i].push_back(limbo::tools::make_vector(observations[j][i]));
                }
            }
            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
                _gp_models[i].mean_function().set_id(i);
                _gp_models[i].compute(samples, obs[i], compute_kernel);
            });
        }

        void optimize_hyperparams()
        {
            _hp_optimize(*this);
        }

        void set_kernel_h_params(const Eigen::VectorXd& kernel_params)
        {
            int curr_size = 0;
            for (int i = 0; i < _dim_out; i++) {
                int p_size = _gp_models[i].kernel_function().h_params_size();
                _gp_models[i].kernel_function().set_h_params(kernel_params.segment(curr_size, p_size));
                curr_size += p_size;
            }
        }

        Eigen::VectorXd kernel_h_params()
        {
            Eigen::VectorXd params;
            for (int i = 0; i < _dim_out; i++) {
                int p_size = _gp_models[i].kernel_function().h_params_size();
                params.conservativeResize(params.size() + p_size);
                params.tail(p_size) = _gp_models[i].kernel_function().h_params();
            }

            return params;
        }

        void set_mean_h_params(const Eigen::VectorXd& mean_params)
        {
            for (int i = 0; i < _dim_out; i++) {
                _gp_models[i].mean_function().set_h_params(mean_params);
            }
        }

        Eigen::VectorXd mean_h_params()
        {
            return _gp_models[0].mean_function().h_params();
        }

        void add_sample(const Eigen::VectorXd& sample, const Eigen::VectorXd& observation)
        {
            if (_gp_models.size() == 0) {
                if (_dim_in != sample.size()) {
                    _dim_in = sample.size();
                }
                if (_dim_out != observation.size()) {
                    _dim_out = observation.size();
                    _gp_models.resize(_dim_out);
                    for (int i = 0; i < _dim_out; i++)
                        _gp_models[i] = GP_t(_dim_in, 1);
                }
            }
            else {
                assert(sample.size() == _dim_in);
                assert(observation.size() == _dim_out);
            }

            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
                _gp_models[i].mean_function().set_id(i);
                _gp_models[i].add_sample(sample, limbo::tools::make_vector(observation[i]));
            });
        }

        std::tuple<Eigen::VectorXd, Eigen::VectorXd> query(const Eigen::VectorXd& v) const
        {
            Eigen::VectorXd mu(_dim_out);
            Eigen::VectorXd sigma(_dim_out);

            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
                Eigen::VectorXd tmp;
                std::tie(tmp, sigma(i)) = _gp_models[i].query(v);
                mu(i) = tmp(0);
            });

            return std::make_tuple(mu, sigma);
        }

        Eigen::VectorXd mu(const Eigen::VectorXd& v) const
        {
            Eigen::VectorXd mu(_dim_out);

            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
                mu(i) = _gp_models[i].mu(v);
            });

            return mu;
        }

        Eigen::VectorXd sigma(const Eigen::VectorXd& v) const
        {
            Eigen::VectorXd sigma(_dim_out);

            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
                sigma(i) = _gp_models[i].sigma(v);
            });

            return sigma;
        }

        /// return the number of dimensions of the input
        int dim_in() const
        {
            assert(_dim_in != -1); // need to compute first !
            return _dim_in;
        }

        /// return the number of dimensions of the output
        int dim_out() const
        {
            assert(_dim_out != -1); // need to compute first !
            return _dim_out;
        }

        /// return the number of samples used to compute the GP
        int nb_samples() const
        {
            assert(_gp_models.size());
            return _gp_models[0].samples().size();
        }

        ///  recomputes the GP
        void recompute(bool update_obs_mean = true, bool update_full_kernel = true)
        {
            tbb::parallel_for(size_t(0), (size_t)_dim_out, size_t(1), [&](size_t i) {
              _gp_models[i].recompute(update_obs_mean, update_full_kernel);
            });
        }

        /// return the list of samples that have been tested so far
        const std::vector<Eigen::VectorXd>& samples() const
        {
            assert(_gp_models.size());
            return _gp_models[0].samples();
        }

        std::vector<GP_t> gp_models() const
        {
            return _gp_models;
        }

        std::vector<GP_t>& gp_models()
        {
            return _gp_models;
        }

    protected:
        std::vector<GP_t> _gp_models;
        int _dim_in, _dim_out;
        HyperParamsOptimizer _hp_optimize;
    };
}

#endif
