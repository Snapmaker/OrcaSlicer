// AdaptivePAProcessor.hpp
// Snapmaker_Orca
//
// Header file for the AdaptivePAProcessor class, responsible for processing G-code layers for the purposes of applying adaptive pressure advance.

#ifndef ADAPTIVEPAPROCESSOR_H
#define ADAPTIVEPAPROCESSOR_H

#include <string>
#include <sstream>
#include <regex>
#include <memory>
#include <map>
#include <vector>
#include "AdaptivePAInterpolator.hpp"

namespace Slic3r {

// Forward declaration of GCode class
class GCode;

/**
 * @brief Class for processing G-code layers with adaptive pressure advance.
 */
class AdaptivePAProcessor {
public:
    /**
     * @brief Constructor for AdaptivePAProcessor.
     *
     * This constructor initializes the AdaptivePAProcessor with a reference to a GCode object.
     * It also initializes the configuration reference, pressure advance interpolation object,
     * and regular expression patterns used for processing the G-code.
     *
     * @param gcodegen A reference to the GCode object that generates the G-code.
     */
    AdaptivePAProcessor(GCode &gcodegen, const std::vector<unsigned int> &tools_used);
    
    /**
     * @brief Processes a layer of G-code and applies adaptive pressure advance.
     *
     * This method processes the G-code for a single layer, identifying the appropriate
     * pressure advance settings and applying them based on the current state and configurations.
     *
     * @param gcode A string containing the G-code for the layer.
     * @return A string containing the processed G-code with adaptive pressure advance applied.
     */
    std::string process_layer(std::string &&gcode);
    
    /**
     * @brief Manually sets adaptive PA internal value.
     *
     * This method manually sets the adaptive PA internally held value.
     * Call this when changing tools or in any other case where the internally assumed last PA value may be incorrect.
     * Only safe while process_layer() is not running on another thread: inside the layer pipeline,
     * emit reset_marker() into the layer G-code instead.
     */
    void resetPreviousPA(double PA){ m_last_predicted_pa = PA; };

    /**
     * @brief In-band equivalent of resetPreviousPA() for G-code that goes through process_layer().
     *
     * The layer pipeline generates layer N+1 while this processor is still working on layer N, so a
     * tool change must not touch the processor from the generator thread. It writes this line right
     * after its own pressure advance command instead; process_layer() applies the reset when it
     * reaches the line, in G-code order, and drops the line from its output.
     *
     * @param PA The pressure advance the tool change has just set.
     * @return The marker line, newline terminated.
     */
    static std::string reset_marker(double PA);

private:
    GCode &m_gcodegen; ///< Reference to the GCode object.
    std::unordered_map<unsigned int, std::unique_ptr<AdaptivePAInterpolator>> m_AdaptivePAInterpolators; ///< Map between Interpolator objects and tool ID's
    const PrintConfig &m_config; ///< Reference to the print configuration.
    double m_last_predicted_pa; ///< Last predicted pressure advance value.
    double m_max_next_feedrate; ///< Maximum feed rate (speed) for the upcomming island. If no speed is found, the previous island speed is used.
    double m_next_feedrate; ///< First feed rate (speed) for the upcomming island.
    double m_current_feedrate; ///< Current, latest feedrate.
    int m_last_extruder_id; ///< Last used extruder ID.

    std::regex m_pa_change_pattern; ///< Regular expression to detect PA_CHANGE pattern.
    std::regex m_g1_f_pattern; ///< Regular expression to detect G1 F pattern.
    std::smatch m_match; ///< Match results for regular expressions.

    /**
     * @brief Get the PA interpolator attached to the specified tool ID.
     *
     * This method manually sets the adaptive PA internally held value.
     * Call this when changing tools or in any other case where the internally assumed last PA value may be incorrect
     *
     * @param An integer with the tool ID for which the PA interpolation model is to be returned.
     * @return The Adaptive PA Interpolator object corresponding to that tool.
     */
    AdaptivePAInterpolator* getInterpolator(unsigned int tool_id);

    /**
     * @brief Applies a reset_marker() line.
     *
     * @return False when the line is not a reset marker.
     */
    bool applyResetMarker(const std::string &line);
};

} // namespace Slic3r

#endif // ADAPTIVEPAPROCESSOR_H
