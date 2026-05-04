import axios from "axios";

export default {

    name: 'hubspot-create',
    description: 'Create Company in Hubspot',
    arguments: {

    },

    handle: async function () {
        const hubspotToken = process.env.HUBSPOT_API_TOKEN;
        if (!hubspotToken) {
            console.error('HUBSPOT_API_TOKEN is not set in the environment variables.');
            return;
        }

        /** TF 09 */
    }
}