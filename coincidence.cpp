//
// Created by Martin Rusilowicz on 18/08/2017.
//

#include <iostream>
#include <fstream>
#include <vector>

#include "coincidence.h"
#include "dataset.h"
#include "elements.h"
#include "parameters.h"
#include "significance.h"
#include "binomial_test.h"
#include "math.h"

#include <Python.h>
#include "omp.h"

#include <iterator>

/**
 * Runs coincidence analysis
 */
int Coincidence::run( DataSet& dataset, /**< Dataset */
		       const std::string& phylogeny,
		       const std::string& path,
		       const std::string& prefix )
{
    //Coincidence::_write_header(dataset);
    const TParameters& options = dataset.get_options();

    std::cerr << "Iterating matrix..." << std::endl;
    const id_lookup<Alpha>& alpha_table = dataset.get_alphas();
    const id_lookup<Edge>& edge_table = dataset.get_edges();

    const double cor_sig = Significance::correct( dataset.get_options().sig_level, dataset.get_options().correction, dataset.get_num_edges());

    //
    // Create matrix
    //
    int num_alphas = static_cast<int>(alpha_table.get_table().size());
    int count      = 0;

    //
    // Determine the maximum phylogenetic distance in given tree and save distance information into hash
    // (Right now, there is only reason to do this if the mode is coincidence)
    //
    std::map<double, std::pair<std::string, std::string>> phylo_dists;
    double max_phylo_dist = 0;
    //
    // Iterate first alpha
    //
    std::cerr << "Running analyses..." << std::endl;
    //Open output file to write to
    std::ofstream analysis;
    std::string analyname = prefix + "_pairs.csv";
    analysis.open(analyname);
    //Write header
    Coincidence::_write_header(dataset, analysis);

    //Set retval; adjust to 0 if/when a coincident pair is identified.
    int returnflag = -1;

    //
    // *** Parallelize ***
    //
    int size_alpha_table = alpha_table.get_table().size();
    #pragma omp parallel for collapse(2)
    for(int yain_count=0; yain_count<size_alpha_table; ++yain_count)
    {
	for(int tain_count=0; tain_count<size_alpha_table; ++tain_count)
        {
	    auto kvp_yain = alpha_table.get_table().begin();
	    auto kvp_tain = alpha_table.get_table().begin();
	    std::advance(kvp_yain, yain_count);
	    std::advance(kvp_tain, tain_count);

	    Alpha& alpha_yain = *kvp_yain->second;
	    const std::map<const Beta*, int>& edges_yain = alpha_yain.get_edges();
	    int num_edges_yain = static_cast<int>(edges_yain.size());

            Alpha& alpha_tain = *kvp_tain->second;
            
	    if (alpha_tain.get_name().compare( alpha_yain.get_name()) <= 0)
            {
                continue;
            }

            const std::map<const Beta*, int>& edges_tain = alpha_tain.get_edges();
            int num_edges_tain = static_cast<int>(edges_tain.size());

	    //
            // Count overlaps & union
	    //
            int overlaps = 0;
	    std::vector<std::string> edges_ovlp;
	    std::vector<std::string> edges_union;

            for (const auto& kvp_edges : edges_yain)
            {
                const Beta& beta = *kvp_edges.first;
                auto it = edges_tain.find( &beta );
                if (it != edges_tain.end())
                {
                    overlaps += 1;
		    edges_ovlp.push_back(beta.get_name());
                }
		edges_union.push_back(beta.get_name());
            }
	    for (const auto& kvp_edges : edges_tain) {
		const Beta& beta = *kvp_edges.first;
		auto it =edges_yain.find( & beta );
		if (it == edges_tain.end()) {
			edges_union.push_back(beta.get_name());
		}
	    }

	    // Count total range
            int total_range = num_edges_yain + num_edges_tain - overlaps;
    	    int       num_observations;
    	    double retval = NAN;
    	    const int max_coincidence = dataset.get_betas().size();

    		switch (options.coin_set_mode)
    		{
        		case ESetMode::INTERSECTION:
        	    	num_observations = total_range;

            		if (num_observations == 0)
            		{
                		if (options.verbose)
                		{
                    			std::cerr << "Rejected (" << alpha_yain.get_name() << ", " << alpha_tain.get_name() << ") because there are no observations." << std::endl;
                		}
                		continue;
            		}
            		break;

        		case ESetMode::FULL:
        		    num_observations = max_coincidence;
        		    break;

        		default:
        		    throw std::logic_error( "Invalid options around SET_MODE_MASK." );
    		}

    		// From this we can work out the chance of alpha/beta occurring at the same time
    		double chance_i = static_cast<double>(num_edges_yain) / static_cast<double>(num_observations);
    		double chance_j = static_cast<double>(num_edges_tain) / static_cast<double>(num_observations);

    		double not_cross_1_chance = static_cast<double>( num_observations - num_edges_yain ) / static_cast<double>(num_observations);
    		double not_cross_2_chance = static_cast<double>( num_observations - num_edges_tain ) / static_cast<double>(num_observations);

    		double rate;
    		int    successes;

    		switch (options.coin_max_mode)
    		{
        		case EMaxMode::AVOID:
        		{
        			successes = total_range - overlaps;   // note that the upper triangle (i,j) is 1&2 whilst the lower triangle (j,i) is 1|2
            			rate      = ( chance_i * not_cross_2_chance ) + ( chance_j * not_cross_1_chance );
            			break;
        		}
        		case EMaxMode::ACCOMPANY:
        		{
            			successes = overlaps;
            			rate      = chance_i * chance_j;
            			break;
        		}
        		default:
        		{
            			throw std::logic_error( "Invalid options around MAX_MODE_MASK." );
        		}
    		}

    		// This causes problems, get rid of it
    		if (rate == 0 || rate == 1.0)
    		{
        		if (options.verbose)
        		{
            			std::cerr << "Rejected (" << alpha_yain.get_name() << ", " << alpha_tain.get_name() << ") because the rate is " << rate << "." << std::endl;
        		}
        		continue;
    		}

    		// Binomial test p-value
    		double p_value = Binomial::test( options.alt_hypothesis, successes, num_observations, rate );

    		if (options.verbose)
    		{
			#pragma omp critical
			{
        			std::cerr << "*******************************" << std::endl;
        			std::cerr << "* yain                " << alpha_yain.get_name() << "." << std::endl;
        			std::cerr << "* tain                " << alpha_tain.get_name() << "." << std::endl;
        			std::cerr << "*------------------------------" << std::endl;
        			std::cerr << "* any_yain            " << num_edges_yain << "." << std::endl;
        			std::cerr << "* any_tain            " << num_edges_tain << "." << std::endl;
        			std::cerr << "* both_of             " << overlaps << "." << std::endl;
        			std::cerr << "* one_of              " << total_range << "." << std::endl;
        			std::cerr << "* max_coincidence     " << max_coincidence << "." << std::endl;
        			std::cerr << "*------------------------------" << std::endl;

        			std::cerr << "*------------------------------" << std::endl;
        			std::cerr << "* chance_i            " << chance_i << "." << std::endl;
        			std::cerr << "* chance_j            " << chance_j << "." << std::endl;
        			std::cerr << "* not_cross_1_chance  " << not_cross_1_chance << "." << std::endl;
        			std::cerr << "* not_cross_2_chance  " << not_cross_2_chance << "." << std::endl;
        			std::cerr << "*------------------------------" << std::endl;
        			std::cerr << "* rate                " << rate << "." << std::endl;
        			std::cerr << "* successes           " << successes << "." << std::endl;
        			std::cerr << "* num_observations    " << num_observations << "." << std::endl;
        			std::cerr << "*------------------------------" << std::endl;
        			std::cerr << "* p_value LESS        " << Binomial::one_sided_less( successes, num_observations, rate ) << "." << std::endl;
        			std::cerr << "* p_value GREATER     " << Binomial::one_sided_greater( successes, num_observations, rate ) << "." << std::endl;
        			std::cerr << "* p_value TWOTAILED   " << Binomial::two_sided( static_cast<uint32_t>(successes), static_cast<uint32_t>(num_observations), rate ) << "." << std::endl;
        			std::cerr << "*******************************" << std::endl;
			}
    		}

    		if (p_value > cor_sig)
    		{
        		if (options.verbose)
        		{
				#pragma omp critical
            			std::cerr << "Rejected (" << alpha_yain.get_name() << ", " << alpha_tain.get_name() << ") because it isn't significant with p = " << p_value << "." << std::endl;
        		}

        		if (!options.output_all)
        		{
            			continue;
        		}
    		}
    		else
    		{
        		if (options.verbose)
        		{
				#pragma omp critical
            			std::cerr << "Accepted (" << alpha_yain.get_name() << ", " << alpha_tain.get_name() << ") because it is significant with p = " << p_value << "." << std::endl;
        		}
    		}

    		//Result is significant: calculate secondaries for coincidence or avoidance, depending on what the user called for
    		retval = p_value;
		returnflag = 0;
    		switch (options.coin_max_mode)
    		{
        		case EMaxMode::ACCOMPANY:
        		{
    				if (options.verbose) {
					std::cerr << "Calling: calc_secondaries" << std::endl;
				}
				double syn_sums = 0;
        			//Cycle through edges_yain, find values also in edges_tain
        			for(const auto &p : edges_ovlp) {
                			//Synthetic distance
                			Edge edge1 = edge_table.find_id(alpha_yain.get_name()+"-"+(p.c_str()));
                			Edge edge2 = edge_table.find_id(alpha_tain.get_name()+"-"+(p.c_str()));
                			syn_sums += abs(edge1.get_weight()-edge2.get_weight());
        			}
        			double avg_syndist = syn_sums/edges_ovlp.size();
				if (options.verbose) {
					std::cerr << "Returning: calc_secondaries" << std::endl;
				}
				
				#pragma omp critical
				{
    					analysis << alpha_yain.get_name()
        	      			<< "\t" << alpha_tain.get_name()
        	      			<< "\t" << p_value
		      			<< "\t" << avg_syndist
        	      			<< "\t" << successes
        	      			<< "\t" << num_observations
        	      			<< "\t" << rate
        	      			<< "\t" << static_cast<int>(rate * num_observations + 0.5)
        	      			<< "\t" << num_edges_yain
        	      			<< "\t" << num_edges_tain
        	      			<< "\t" << chance_i
        	      			<< "\t" << chance_j
        	      			<< std::endl;
				}
				if (options.verbose) {
					#pragma omp critical
					std::cerr << "All done; next case." << std::endl;
				}
				break;
			}
			case EMaxMode::AVOID:
        		{
				//TODO- are there any interesting secondaries to do for avoidance data?
				#pragma omp critical
				{
					analysis << alpha_yain.get_name()
		     			<< "\t" << alpha_tain.get_name()
                     			<< "\t" << p_value
                     			<< "\t" << successes
                     			<< "\t" << num_observations
                     			<< "\t" << rate
                     			<< "\t" << static_cast<int>(rate * num_observations + 0.5)
                     			<< "\t" << num_edges_yain
                     			<< "\t" << num_edges_tain
                     			<< "\t" << chance_i
                     			<< "\t" << chance_j
                     			<< std::endl;
				}
				break;
			}
			default:
        		{
				#pragma omp critical
            			throw std::logic_error( "Invalid options around MAX_MODE_MASK." );
        		}
   		}

                dataset._generate_coincident_edge(alpha_yain, alpha_tain, retval);;
	}
    }
    //
    // *** End parallelize ***
    //
    analysis.close();
    return(returnflag);
}





/**
 * Writes the header
 */
void Coincidence::_write_header(const DataSet& dataset, std::ofstream& analysis)
{
    const TParameters& options = dataset.get_options();

    switch (options.coin_max_mode)
    {
    	case EMaxMode::ACCOMPANY:
    	{
    		analysis << "Source"
        		<< "\t" << "Target"
        	      	<< "\t" << "p"
		      	<< "\t" << "Avg synthetic distance"
        	      	<< "\t" << "successes"
        	      	<< "\t" << "observations"
        	      	<< "\t" << "rate"
        	      	<< "\t" << "expected"
        	      	<< "\t" << "total source"
        	      	<< "\t" << "total target"
        	      	<< "\t" << "fraction source"
        	      	<< "\t" << "fraction target"
        	      	<< std::endl;
		break;
	}
	case EMaxMode::AVOID:
	{
		analysis << "Source"
                        << "\t" << "Target"
                        << "\t" << "p"
                        << "\t" << "successes"
                        << "\t" << "observations"
                        << "\t" << "rate"
                        << "\t" << "expected"
                        << "\t" << "total source"
                        << "\t" << "total target"
                        << "\t" << "fraction source"
                        << "\t" << "fraction target"
                        << std::endl;
		break;
	}
	default:
        {   
            throw std::logic_error( "Invalid options around MAX_MODE_MASK.");
        }
   }
	
}


