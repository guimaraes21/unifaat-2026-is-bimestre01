import axios from "axios";

export default {

    name: 'hubspot-list',
    description: 'Contact Hubspot',
    arguments: {

    },

    handle: async function () {
        const hubspotToken = process.env.HUBSPOT_API_TOKEN;
        if (!hubspotToken) {
            console.error('HUBSPOT_API_TOKEN is not set in the environment variables.');
            return;
        }

        const query = {
            "properties": "firstname,lastname,email"
        }

        const queryString = new URLSearchParams(query).toString();

        const url = `https://api.hubapi.com/crm/v3/objects/contacts?${queryString}`;

        console.log(url);

        const response = await axios.get(url, {
            headers: {
                'Authorization': `Bearer ${hubspotToken}`
            }
        });

        console.log(response.data);
    }
}